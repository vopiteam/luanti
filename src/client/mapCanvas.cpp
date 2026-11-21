// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025 VOPI Team

#include "mapCanvas.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include "client/node_visuals.h"
#include "constants.h"
#include "filesys.h"
#include "log.h"
#include "map.h"
#include "mapblock.h"
#include "mapnode.h"
#include "nodedef.h"
#include "porting.h"
#include "profiler.h"
#include "serialization.h"
#include "util/numeric.h"
#include "util/serialize.h"

namespace {

// In-RAM resident-tile cap (hard backstop only — the regular bound is the
// distance-based evictFarTiles pass). 128 * 96 KiB = ~12 MiB. The map render
// window at the widest zoom (1024 nodes => 9x9 = 81 explored tiles, ~121
// hinted with slack) plus the harvest keep window (~25-49 tiles) can brush
// against this cap; pass 2 below prefers non-hinted tiles so the windows
// being actively read are evicted last.
constexpr size_t MAX_RESIDENT_TILES = 128;

// How long a render-window keep hint stays in force after the map UI last
// rendered (ms). Outlives a few rebuild throttles (REBUILD_STALE_MS = 2 s in
// guiMapElement.cpp), then far tiles get evicted.
constexpr u64 RENDER_HINT_TTL_MS = 10000;

// Flush dirty tiles to disk at most this often (seconds).
constexpr f32 FLUSH_INTERVAL_S = 10.0f;

// Saving a dirty tile costs a 96 KiB zstd compression + a file write on the
// MAIN thread, so both the eviction pass and the periodic flush bound how
// many tiles they save per call; the remainder stays resident/dirty and is
// drained by the following calls (sweeps run every 0.5-3 s; durability is
// soft — the authoritative data is the world itself).
constexpr u32 MAX_TILE_SAVES_PER_CALL = 4;

// On-disk tile header.
const char TILE_MAGIC[4] = {'K', 'C', 'M', 'T'};
constexpr u8 TILE_VERSION = 1;

// Raw (uncompressed) payload size: colour[] as u32 + height[] as s16.
constexpr size_t PAYLOAD_BYTES =
	(size_t)MapTile::CELLS * 4 + (size_t)MapTile::CELLS * 2;

// Bounded decompression sink: caps the output so a corrupt or maliciously
// crafted (zstd "zip-bomb") fog tile can't OOM the client. Bytes past the cap
// are dropped; loadTile's exact-size check then rejects the tile. The cap is
// PAYLOAD_BYTES + 1 so a valid tile fits exactly while an oversized stream
// trips the size mismatch. (decompressZstd ignores the stream state, so it
// cannot be stopped early — but the in-RAM buffer stays bounded.)
class CappedSink : public std::streambuf {
public:
	explicit CappedSink(size_t cap) : m_cap(cap) {}
	const std::string &str() const { return m_data; }
protected:
	std::streamsize xsputn(const char *s, std::streamsize n) override
	{
		const size_t room = m_data.size() < m_cap ? m_cap - m_data.size() : 0;
		if (room)
			m_data.append(s, (size_t)n < room ? (size_t)n : room);
		return n;
	}
	int_type overflow(int_type c) override
	{
		if (c != traits_type::eof() && m_data.size() < m_cap)
			m_data.push_back((char)c);
		return c;
	}
private:
	size_t m_cap;
	std::string m_data;
};

} // namespace

// ---------------------------------------------------------------------------
// Column-scan helper (used by the persistent-canvas harvest)
// ---------------------------------------------------------------------------

bool scanSurfaceColumn(Map &map, const NodeDefManager *ndef, s16 wx, s16 wz,
		s16 y_top, s16 y_bottom, u32 &out_argb, s16 &out_height)
{
	// Scan top-down, caching the current MapBlock so we don't do a full block
	// lookup per node, and skipping whole unloaded blocks in one jump.
	MapBlock *block = nullptr;
	v3s16 cached_bp(-32768, -32768, -32768);

	// Whether the node directly above the current scan position is confirmed
	// LOADED empty space (real air). The surface we finally pick is trustworthy
	// only if loaded air sits directly above it — that proves nothing solid is
	// hidden higher up. If we instead reached a node by jumping over an unloaded
	// or ungenerated block, a real (higher) surface may still be streaming in
	// there and the node we found is likely a DEEPER layer (e.g. red sandstone
	// under desert sand). Recording that would freeze a wrong colour into the
	// persistent canvas (skip-recorded never revisits it), so we bail and let a
	// later sweep record the column once it is fully loaded.
	bool air_above = false;

	for (s16 wy = y_top; wy >= y_bottom; wy--) {
		const v3s16 wp(wx, wy, wz);
		const v3s16 bp = getNodeBlockPos(wp);
		if (bp != cached_bp) {
			cached_bp = bp;
			block = map.getBlockNoCreateNoEx(bp);
		}
		if (!block) {
			wy = bp.Y * MAP_BLOCKSIZE; // loop's wy-- lands at this block's bottom-1
			air_above = false;         // jumped an unloaded block: above is not loaded
			continue;
		}

		MapNode n = block->getNodeNoCheck(wp - bp * MAP_BLOCKSIZE);
		content_t c = n.getContent();
		if (c == CONTENT_AIR) {
			air_above = true;          // real, loaded air above
			continue;
		}
		if (c == CONTENT_IGNORE) {
			air_above = false;         // ungenerated node: treat like unloaded
			continue;
		}

		const ContentFeatures &f = ndef->get(c);
		if (f.drawtype == NDT_AIRLIKE) {
			air_above = true;
			continue;
		}

		// Topmost solid node. Trust it as the surface only if confirmed-loaded
		// air sits directly above; otherwise the true surface may be hidden in an
		// unloaded block overhead and this is a deeper layer — skip, retry later.
		if (!air_above)
			return false;

		// Representative top colour of the node (same recipe as the minimap).
		video::SColor tilecolor(255, 255, 255, 255);
		const TileDef &tile = f.tiledef[0];
		const TileDef &overlay = f.tiledef_overlay[0];
		if (!overlay.name.empty() && overlay.has_color) {
			tilecolor = overlay.color;
		} else if (overlay.name.empty() && tile.has_color) {
			tilecolor = tile.color;
		} else if (f.visuals) {
			f.visuals->getColor(n.param2, &tilecolor);
		}
		if (f.visuals) {
			const video::SColor &mc = f.visuals->minimap_color;
			tilecolor.setRed(tilecolor.getRed() * mc.getRed() / 255);
			tilecolor.setGreen(tilecolor.getGreen() * mc.getGreen() / 255);
			tilecolor.setBlue(tilecolor.getBlue() * mc.getBlue() / 255);
		}
		tilecolor.setAlpha(255); // explored cells are always opaque (not fog)

		out_argb = tilecolor.color;
		out_height = wy;
		return true;
	}

	return false;
}

// ---------------------------------------------------------------------------
// MapCanvas
// ---------------------------------------------------------------------------

MapCanvas::MapCanvas(std::string dir) :
	m_dir(std::move(dir))
{
}

MapCanvas::~MapCanvas()
{
	flushAll();
}

s16 MapCanvas::tileCoord(s16 w)
{
	return getContainerPos(w, (s16)MAPCANVAS_TILE_NODES);
}

std::string MapCanvas::tilePath(s16 tx, s16 tz) const
{
	return m_dir + DIR_DELIM + "t_" + std::to_string(tx) + "_" +
		std::to_string(tz) + ".bin";
}

void MapCanvas::setCell(s16 wx, s16 wz, u32 argb, s16 height)
{
	const s16 tx = tileCoord(wx);
	const s16 tz = tileCoord(wz);
	MapTile *t = getOrCreateTile(tx, tz);

	const s32 ox = wx & (MAPCANVAS_TILE_NODES - 1);
	const s32 oz = wz & (MAPCANVAS_TILE_NODES - 1);
	const s32 idx = oz * MAPCANVAS_TILE_NODES + ox;

	t->colour[idx] = argb;
	t->height[idx] = height;
	t->dirty = true;
}

const MapTile *MapCanvas::findTileReadonly(s16 tx, s16 tz)
{
	auto it = m_tiles.find(v2s16(tx, tz));
	if (it != m_tiles.end())
		return it->second.get();

	// Cold miss. Cheaply check the file exists BEFORE allocating a 96 KiB tile:
	// most lookups for unexplored area miss, and a 96 KiB alloc + memset per
	// miss is exactly what made the render assemble pass expensive.
#if IS_VOPI_ENGINE
	// A canvas in memory only has no file to look for; the tiles it evicts
	// are forgotten
	if (m_dir.empty())
		return nullptr;
#endif
	if (!fs::PathExists(tilePath(tx, tz)))
		return nullptr;

	auto tile = std::make_unique<MapTile>();
	{
		ScopeProfiler sp(g_profiler,
			"Client: map canvas tile load [ms]", SPT_AVG, PRECISION_MILLI);
		if (!loadTile(tx, tz, *tile))
			return nullptr;
	}

	const MapTile *raw = tile.get();
	m_tiles[v2s16(tx, tz)] = std::move(tile);
	// No eviction here — see evictFarTiles() / the pointer-validity contract.
	return raw;
}

MapTile *MapCanvas::getOrCreateTile(s16 tx, s16 tz)
{
	auto it = m_tiles.find(v2s16(tx, tz));
	if (it != m_tiles.end())
		return it->second.get();

	g_profiler->add("Client: map canvas create insert [#]", 1);
	auto tile = std::make_unique<MapTile>();
	// Seed from disk if this tile was explored in a previous session, so we
	// extend rather than overwrite saved progress.
	loadTile(tx, tz, *tile);

	MapTile *raw = tile.get();
	m_tiles[v2s16(tx, tz)] = std::move(tile);
	// No eviction here — see evictFarTiles() / the pointer-validity contract.
	return raw;
}

void MapCanvas::setRenderWindowHint(s16 center_tx, s16 center_tz, s16 radius_tiles)
{
	m_hint_center = v2s16(center_tx, center_tz);
	m_hint_radius = radius_tiles;
	m_hint_set_ms = porting::getTimeMs();
}

bool MapCanvas::overResidentCap() const
{
	return m_tiles.size() > MAX_RESIDENT_TILES;
}

void MapCanvas::evictFarTiles(s16 center_tx, s16 center_tz, s16 keep_radius_tiles)
{
	const auto tile_dist = [&](const v2s16 &p) -> s32 {
		return std::max(std::abs((s32)p.X - center_tx),
			std::abs((s32)p.Y - center_tz));
	};

	const bool hint_active = m_hint_radius > 0 &&
		porting::getTimeMs() - m_hint_set_ms < RENDER_HINT_TTL_MS;
	const auto in_hint_window = [&](const v2s16 &p) -> bool {
		if (!hint_active)
			return false;
		return std::max(std::abs((s32)p.X - m_hint_center.X),
			std::abs((s32)p.Y - m_hint_center.Y)) <= m_hint_radius;
	};

	// Saving a dirty tile is main-thread zstd + file I/O; bound it per call.
	// A dirty tile that would exceed the budget is NOT evicted this call — it
	// stays resident and is drained by the next sweep (pass 2's hard cap is
	// exempt: RAM bounding there outranks the I/O smoothing).
	u32 saves_left = MAX_TILE_SAVES_PER_CALL;
	const auto evict_tile = [&](decltype(m_tiles)::iterator it, bool obey_save_budget)
			-> decltype(m_tiles)::iterator {
		if (it->second->dirty) {
			if (obey_save_budget && saves_left == 0)
				return std::next(it); // keep for a later call
			if (saves_left > 0)
				saves_left--;
			ScopeProfiler sp(g_profiler,
				"Client: map canvas evict save [ms]", SPT_AVG, PRECISION_MILLI);
			saveTile(it->first.X, it->first.Y, *it->second);
		}
		g_profiler->add("Client: map canvas evictions [#]", 1);
		return m_tiles.erase(it);
	};

	// Pass 1: drop everything outside the caller's working window (but keep
	// the map UI's announced render window).
	for (auto it = m_tiles.begin(); it != m_tiles.end(); ) {
		if (tile_dist(it->first) > keep_radius_tiles && !in_hint_window(it->first))
			it = evict_tile(it, true);
		else
			++it;
	}

	// Pass 2 (hard-cap backstop): a huge working window (e.g. the map zoomed
	// all the way out) can itself exceed the resident cap — evict the farthest
	// tiles down to the cap so RAM stays bounded either way. Prefer non-hinted
	// tiles so the windows being actively read are evicted last.
	while (m_tiles.size() > MAX_RESIDENT_TILES) {
		auto farthest = m_tiles.end();
		s32 best = -1;
		bool best_hinted = true;
		for (auto it = m_tiles.begin(); it != m_tiles.end(); ++it) {
			const bool hinted = in_hint_window(it->first);
			const s32 d = tile_dist(it->first);
			// A non-hinted tile always outranks a hinted one; among equals,
			// take the farthest from the player.
			if ((best_hinted && !hinted) || (best_hinted == hinted && d > best)) {
				best = d;
				best_hinted = hinted;
				farthest = it;
			}
		}
		if (farthest == m_tiles.end())
			break;
		evict_tile(farthest, false);
	}
}

void MapCanvas::maybeFlush(f32 dtime)
{
	m_flush_timer += dtime;
	if (m_flush_timer < FLUSH_INTERVAL_S)
		return;

	// Budgeted flush: writing a tile is main-thread zstd + file I/O, so write
	// at most a few per call instead of every dirty tile in one frame. If
	// dirty tiles remain, retry shortly rather than waiting a full interval.
	ScopeProfiler sp(g_profiler,
		"Client: map canvas flush [ms]", SPT_AVG, PRECISION_MILLI);
	u32 written = 0;
	bool dirty_left = false;
	for (auto &entry : m_tiles) {
		if (!entry.second->dirty)
			continue;
		if (written >= MAX_TILE_SAVES_PER_CALL) {
			dirty_left = true;
			break;
		}
		saveTile(entry.first.X, entry.first.Y, *entry.second);
		entry.second->dirty = false;
		written++;
	}
	g_profiler->avg("Client: map canvas flush tiles written [#]", written);

	m_flush_timer = dirty_left ? FLUSH_INTERVAL_S - 1.0f : 0.0f;
}

void MapCanvas::flushAll()
{
	// Unbudgeted: writes every dirty tile. Only for shutdown (destructor),
	// where losing the in-RAM canvas matters more than one slow frame.
	for (auto &entry : m_tiles) {
		if (entry.second->dirty) {
			saveTile(entry.first.X, entry.first.Y, *entry.second);
			entry.second->dirty = false;
		}
	}
}

void MapCanvas::saveTile(s16 tx, s16 tz, const MapTile &t) const
{
#if IS_VOPI_ENGINE
	// No directory: the canvas lives in memory only
	if (m_dir.empty())
		return;
#endif
	if (!m_dir_ready) {
		if (!fs::CreateAllDirs(m_dir)) {
			warningstream << "MapCanvas: cannot create dir " << m_dir << std::endl;
			return;
		}
		const_cast<MapCanvas *>(this)->m_dir_ready = true;
	}

	// Build raw payload: colour[] then height[], big-endian (network byte
	// order) via writeU32/writeS16 — read back symmetrically by loadTile.
	std::string raw;
	raw.resize(PAYLOAD_BYTES);
	char *p = raw.data();
	for (s32 i = 0; i < MapTile::CELLS; i++) {
		writeU32((u8 *)p, t.colour[i]);
		p += 4;
	}
	for (s32 i = 0; i < MapTile::CELLS; i++) {
		writeS16((u8 *)p, t.height[i]);
		p += 2;
	}

	std::ostringstream oss(std::ios::binary);
	oss.write(TILE_MAGIC, 4);
	oss.put((char)TILE_VERSION);
	u8 hdr[2];
	writeS16(hdr, tx); oss.write((char *)hdr, 2);
	writeS16(hdr, tz); oss.write((char *)hdr, 2);
	compressZstd(raw, oss);

	if (!fs::safeWriteToFile(tilePath(tx, tz), oss.str()))
		warningstream << "MapCanvas: failed to write tile " << tx << "," << tz << std::endl;
}

bool MapCanvas::loadTile(s16 tx, s16 tz, MapTile &out) const
{
#if IS_VOPI_ENGINE
	if (m_dir.empty())
		return false;
#endif
	const std::string path = tilePath(tx, tz);
	if (!fs::PathExists(path))
		return false;

	std::string data;
	if (!fs::ReadFile(path, data) || data.size() < 9)
		return false;
	if (memcmp(data.data(), TILE_MAGIC, 4) != 0 || (u8)data[4] != TILE_VERSION) {
		warningstream << "MapCanvas: bad header in " << path << std::endl;
		return false;
	}

	// Decompress the payload that follows the 9-byte header, into a size-capped
	// sink so a malformed/oversized tile can't OOM (see CappedSink).
	std::istringstream iss(data.substr(9), std::ios::binary);
	CappedSink sink(PAYLOAD_BYTES + 1);
	std::ostream raw(&sink);
	try {
		decompressZstd(iss, raw);
	} catch (...) {
		warningstream << "MapCanvas: decompress failed for " << path << std::endl;
		return false;
	}
	const std::string &payload = sink.str();
	if (payload.size() != PAYLOAD_BYTES) {
		warningstream << "MapCanvas: bad payload size in " << path << std::endl;
		return false;
	}

	const u8 *p = (const u8 *)payload.data();
	for (s32 i = 0; i < MapTile::CELLS; i++) {
		out.colour[i] = readU32(p);
		p += 4;
	}
	for (s32 i = 0; i < MapTile::CELLS; i++) {
		out.height[i] = readS16(p);
		p += 2;
	}
	return true;
}
