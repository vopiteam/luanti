// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mg_biome_terrain.h"

#if IS_VOPI_ENGINE

#include "mapgen_valleys.h"
#include "constants.h"
#include "noise.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {

constexpr size_t HEIGHT_CACHE_LIMIT = 4096;
constexpr size_t COLUMN_CACHE_LIMIT = 32768;
constexpr size_t CAP_CACHE_LIMIT = 32768;
constexpr size_t CAP_ROW_CACHE_LIMIT = 32768;
constexpr size_t CLIMATE_CACHE_LIMIT = 32768;
constexpr size_t VOXEL_CACHE_LIMIT = 262144;
constexpr size_t TOP_CACHE_LIMIT = 16384;
constexpr size_t COMPONENT_LIMIT = 65536;

u64 columnKey(s32 x, s32 z)
{
	return (static_cast<u64>(static_cast<u32>(x)) << 32) |
		static_cast<u32>(z);
}

// Value noise is bounded by one, and interpolation does not increase that
// bound. Absolute amplitudes also cover negative persistence and scale.
float noiseMagnitude(const NoiseParams &np)
{
	double amplitude = 1.0;
	double total = 0.0;
	for (u16 i = 0; i < np.octaves; ++i) {
		total += amplitude;
		amplitude *= std::fabs(np.persist);
	}
	return std::fabs(np.offset) + std::fabs(np.scale) * total;
}

struct TerrainPoint {
	s32 x, y, z;
	bool operator==(const TerrainPoint &other) const
	{
		return x == other.x && y == other.y && z == other.z;
	}
};

struct TerrainPointHash {
	size_t operator()(const TerrainPoint &p) const
	{
		u64 h = columnKey(p.x, p.z);
		h ^= static_cast<u64>(static_cast<u32>(p.y)) * 0x9e3779b97f4a7c15ULL;
		h ^= h >> 30;
		h *= 0xbf58476d1ce4e5b9ULL;
		h ^= h >> 27;
		return static_cast<size_t>(h ^ (h >> 31));
	}
};

struct TerrainChunk {
	TerrainPoint min;
	TerrainPoint max;
};

s32 chunkMinimum(s32 coordinate, s32 blocks)
{
	const s32 size = blocks * MAP_BLOCKSIZE;
	const s32 offset = (-blocks / 2) * MAP_BLOCKSIZE;
	s32 remainder = (coordinate - offset) % size;
	if (remainder < 0)
		remainder += size;
	return coordinate - remainder;
}

struct ValleysTerrainParams {
	s32 seed;
	ValleysColumnParams column;
	float water_level;
	s32 floor_y;
	bool mountains;
	bool remove_floaters;
	TerrainPoint chunk_blocks;
	float mountain_cap;
	float mountain_cap_height;
	s32 mountain_cap_reach;
	float mountain_noise_max;
	float fill_noise_magnitude;
	float mountain_noise_magnitude;
	NoiseParams fill;
	NoiseParams slope;
	NoiseParams rivers;
	NoiseParams terrain_height;
	NoiseParams valley_depth;
	NoiseParams valley_profile;
	NoiseParams mountain;
	NoiseParams mountain_height;
	NoiseParams wetland_pools;

	explicit ValleysTerrainParams(const MapgenValleysParams &p) :
		seed(static_cast<s32>(p.seed)),
		column(p),
		water_level(p.water_level),
		floor_y(p.floor_y),
		mountains(p.spflags & MGVALLEYS_MOUNTAINS),
		remove_floaters(p.spflags & MGVALLEYS_REMOVE_FLOATERS),
		chunk_blocks{std::max<s32>(p.chunksize.X, 1),
			std::max<s32>(p.chunksize.Y, 1), std::max<s32>(p.chunksize.Z, 1)},
		mountain_cap(p.mountain_cap),
		mountain_cap_height(std::fmax(static_cast<float>(p.mountain_cap_height), 1.0f)),
		mountain_cap_reach(std::min<u16>(p.mountain_cap_reach, 32)),
		fill(p.np_inter_valley_fill),
		slope(p.np_inter_valley_slope),
		rivers(p.np_rivers),
		terrain_height(p.np_terrain_height),
		valley_depth(p.np_valley_depth),
		valley_profile(p.np_valley_profile),
		mountain(p.np_mountain),
		mountain_height(p.np_mountain_height),
		wetland_pools(p.np_wetland_pools)
	{
		// Match the gate used by MapgenValleys::generateTerrain, including its
		// disabled-mountain fast path. The more conservative bounds below are
		// used only to bound the independent vertical search.
		float octaves_max = mountain.persist == 1.0f ? mountain.octaves :
			(1.0f - std::pow(mountain.persist, static_cast<float>(mountain.octaves))) /
			(1.0f - mountain.persist);
		mountain_noise_max = mountain.offset + std::fabs(mountain.scale) * octaves_max;
		mountains = mountains && mountain_noise_max > 0.0f;
		fill_noise_magnitude = noiseMagnitude(fill);
		mountain_noise_magnitude = noiseMagnitude(mountain);
	}
};

// The 2D column comes from the generator's own calcValleysColumn; the 3D
// surface, the density with the mountain body and its cap, the solid
// floor and the floating piece removal, is written a
// second time here for one column instead of a mapchunk, and the unit
// test testValleysSurfaceModel compares it with generation over whole
// mapchunks of the game's profile: a change to either side that the
// other does not follow fails there.
class ValleysBiomeTerrainSampler final : public BiomeTerrainSampler {
public:
	explicit ValleysBiomeTerrainSampler(const MapgenValleysParams &params) :
		m_params(params)
	{
	}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::unique_ptr<BiomeTerrainSampler>(new ValleysBiomeTerrainSampler(m_params));
	}

	float sampleHeight(v2s16 pos) const override;
	bool sampleClimate(v2s16 pos, BiomeClimateContext &out) const override;

	void beginChunk() override
	{
		// Evict between chunks rather than in the middle of one: a mapchunk
		// with its halo needs well under half of each budget, so a cache
		// that is at most half full keeps the whole chunk from the terrain
		// pass through biome selection.
		if (m_climates.size() > CLIMATE_CACHE_LIMIT / 2)
			m_climates.clear();
		if (m_columns.size() > COLUMN_CACHE_LIMIT / 2)
			m_columns.clear();
		if (m_cap_rows.size() > CAP_ROW_CACHE_LIMIT / 2)
			m_cap_rows.clear();
		if (m_caps.size() > CAP_CACHE_LIMIT / 2)
			m_caps.clear();
	}

private:
	explicit ValleysBiomeTerrainSampler(const ValleysTerrainParams &params) :
		m_params(params)
	{
	}

	// The form's body stays 0 in the column: it reads the cap, the feet
	// of the columns within reach, so sampleClimate fills it and caches it
	// with the climate context.
	struct Column {
		float surface;
		float bank;
		float slope;
		float mountain_gate;
		float mountain_height;
		float foot;
		BiomeTerrainForm form;
	};
	enum Voxel : u8 { AIR, SOLID, KEPT, REMOVED };
	float modelHeight(s32 x, s32 z) const;
	Column columnAt(s32 x, s32 z) const;
	TerrainChunk chunkAt(const TerrainPoint &point) const;
	float densityUpperAt(const Column &column) const;
	s32 upperAt(const Column &column) const;
	float densityAt(const TerrainPoint &point, const Column &column) const;
	bool naturalSolid(const TerrainPoint &point) const;
	float floaterFloor(const TerrainPoint &point) const;
	s32 initialTop(s32 x, s32 z, const TerrainChunk &chunk) const;
	bool retained(const TerrainPoint &point) const;
	bool seedsRemoval(const TerrainPoint &top, const TerrainChunk &chunk) const;
	void markVoxel(const TerrainPoint &point, Voxel mark) const;
	void trimColumnCaches() const;
	void trimSurfaceCaches() const;
	float capRowAt(s32 x, s32 z) const;
	float capAt(s32 x, s32 z) const;
	float capLift(float delta, float foot) const;
	float bodyHeightAt(s32 x, s32 z, const Column &column) const;
	const ValleysTerrainParams m_params;
	mutable std::unordered_map<u64, float> m_heights;
	mutable std::unordered_map<u64, Column> m_columns;
	mutable std::unordered_map<u64, float> m_caps;
	mutable std::unordered_map<u64, float> m_cap_rows;
	mutable std::unordered_map<u64, BiomeClimateContext> m_climates;
	mutable std::unordered_map<TerrainPoint, Voxel, TerrainPointHash> m_voxels;
	mutable std::unordered_map<TerrainPoint, s32, TerrainPointHash> m_tops;
};

ValleysBiomeTerrainSampler::Column ValleysBiomeTerrainSampler::columnAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_columns.find(key);
	if (found != m_columns.end())
		return found->second;

	// The generator's own column model on the scalar noise of this column.
	// No node data, climate adjustments or mapgen noise buffers enter.
	const auto &p = m_params;
	const ValleysColumn column = calcValleysColumn(p.column,
		NoiseFractal2D(&p.slope, x, z, p.seed),
		NoiseFractal2D(&p.rivers, x, z, p.seed),
		NoiseFractal2D(&p.terrain_height, x, z, p.seed),
		NoiseFractal2D(&p.valley_depth, x, z, p.seed),
		NoiseFractal2D(&p.valley_profile, x, z, p.seed),
		p.column.wetlands ?
			NoiseFractal2D(&p.wetland_pools, x, z, p.seed) : 0.0f);
	Column c{column.surface_y, column.base, column.slope, 0.0f, 0.0f, 0.0f,
		{column.region_level, column.valley_depth, column.valley_pos, 0.0f, 0.0f,
			column.wetland}};
	if (p.mountains) {
		c.mountain_height = NoiseFractal2D(&p.mountain_height, x, z, p.seed);
		if (c.mountain_height > 0.0f) {
			c.mountain_gate = valleysMountainGate(column, p.column.mountain_river_width);
			c.form.mountain = c.mountain_height * c.mountain_gate;
			if (c.mountain_gate > 0.0f)
				c.foot = valleysMountainFoot(p.mountain, x, z, c.surface,
					c.mountain_gate, p.seed);
		}
	}
	if (m_columns.size() < COLUMN_CACHE_LIMIT)
		m_columns.emplace(key, c);
	return c;
}

// The lift of the cap at delta over the ground: the threshold drops by
// 'mountain_cap' times the tapered foot, most at half 'mountain_cap_height'
// above the ground, nothing at the ground and at the cap height. The
// generator's own cap, written a second time here.
float ValleysBiomeTerrainSampler::capLift(float delta, float foot) const
{
	const auto &p = m_params;
	if (!(delta > 0.0f && delta < p.mountain_cap_height) || p.mountain_cap == 0.0f)
		return 0.0f;
	const float t = delta / p.mountain_cap_height;
	return p.mountain_cap * foot * 4.0f * t * (1.0f - t);
}

// The height the mountain body reaches over the terrain in a column: its
// density at the node nearest each sample, the noise with the cap through
// the gate less the node's height over the terrain divided by the
// mountain height, as the generator evaluates it, sampled at eight steps
// from the highest point it can be positive at down to the terrain, and
// the topmost positive sample refined by three bisections. Within a few
// nodes of what the 3D model finds voxel by voxel, at a dozen noise
// samples, and a function of the noise alone, so selection and queries
// agree. The cap is the strongest tapered foot within reach, so the skirt
// a body spreads over its feet counts; the fill relief does not. Costs
// the columns of the neighbourhood once per chunk, as the generator's
// feet do.
float ValleysBiomeTerrainSampler::bodyHeightAt(s32 x, s32 z, const Column &c) const
{
	const auto &p = m_params;
	const float foot = p.mountain_cap != 0.0f ? capAt(x, z) : 0.0f;
	const float reach = std::fmax(
		p.mountain_noise_max * c.mountain_gate * c.mountain_height,
		foot > 0.0f ? p.mountain_cap_height : 0.0f);
	if (!(reach > 0.0f))
		return 0.0f;
	auto density = [&](float delta) {
		const float y = std::floor(c.surface + delta + 0.5f);
		const float node_delta = y - c.surface;
		return (NoiseFractal3D(&p.mountain, x, y, z, p.seed) +
			capLift(node_delta, foot)) * c.mountain_gate -
			node_delta / c.mountain_height;
	};
	constexpr int STEPS = 8;
	float below = -1.0f;
	float above = reach;
	for (int k = STEPS - 1; k >= 0; --k) {
		const float delta = reach * static_cast<float>(k) / STEPS;
		if (density(delta) > 0.0f) {
			below = delta;
			break;
		}
		above = delta;
	}
	if (below < 0.0f)
		return 0.0f;
	for (int i = 0; i < 3; ++i) {
		const float mid = 0.5f * (below + above);
		if (density(mid) > 0.0f)
			below = mid;
		else
			above = mid;
	}
	return below;
}

float ValleysBiomeTerrainSampler::capRowAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_cap_rows.find(key);
	if (found != m_cap_rows.end())
		return found->second;

	const s32 reach = m_params.mountain_cap_reach;
	float taper = 1.0f / static_cast<float>(reach + 1);
	float row = 0.0f;
	for (s32 dx = -reach; dx <= reach; ++dx)
		row = std::fmax(row, columnAt(x + dx, z).foot *
			(1.0f - static_cast<float>(std::abs(dx)) * taper));
	if (m_cap_rows.size() < CAP_ROW_CACHE_LIMIT)
		m_cap_rows.emplace(key, row);
	return row;
}

float ValleysBiomeTerrainSampler::capAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_caps.find(key);
	if (found != m_caps.end())
		return found->second;

	const s32 reach = m_params.mountain_cap_reach;
	float taper = 1.0f / static_cast<float>(reach + 1);
	float foot = 0.0f;
	for (s32 dz = -reach; dz <= reach; ++dz) {
		float row = capRowAt(x, z + dz);
		foot = std::fmax(foot, row * (1.0f - static_cast<float>(std::abs(dz)) * taper));
	}
	if (m_caps.size() < CAP_CACHE_LIMIT)
		m_caps.emplace(key, foot);
	return foot;
}

void ValleysBiomeTerrainSampler::trimColumnCaches() const
{
	if (m_columns.size() >= COLUMN_CACHE_LIMIT)
		m_columns.clear();
	if (m_caps.size() >= CAP_CACHE_LIMIT)
		m_caps.clear();
	if (m_cap_rows.size() >= CAP_ROW_CACHE_LIMIT)
		m_cap_rows.clear();
}

void ValleysBiomeTerrainSampler::trimSurfaceCaches() const
{
	// Never evict while exploring a connected component. Insertion budgets
	// also bound memory for a single unusually expensive height query.
	trimColumnCaches();
	if (m_voxels.size() >= VOXEL_CACHE_LIMIT)
		m_voxels.clear();
	if (m_tops.size() >= TOP_CACHE_LIMIT)
		m_tops.clear();
}

TerrainChunk ValleysBiomeTerrainSampler::chunkAt(const TerrainPoint &point) const
{
	const auto &blocks = m_params.chunk_blocks;
	TerrainPoint min{chunkMinimum(point.x, blocks.x),
		chunkMinimum(point.y, blocks.y), chunkMinimum(point.z, blocks.z)};
	return {min, {min.x + blocks.x * MAP_BLOCKSIZE - 1,
		min.y + blocks.y * MAP_BLOCKSIZE - 1,
		min.z + blocks.z * MAP_BLOCKSIZE - 1}};
}

float ValleysBiomeTerrainSampler::densityUpperAt(const Column &c) const
{
	const auto &p = m_params;
	float upper = c.surface + std::fabs(c.slope) * p.fill_noise_magnitude;
	if (c.mountain_gate > 0.0f && c.mountain_height > 0.0f)
		upper = std::fmax(upper, c.surface + std::fmax(p.mountain_cap_height,
			p.mountain_noise_magnitude * c.mountain_gate * c.mountain_height));
	return upper;
}

s32 ValleysBiomeTerrainSampler::upperAt(const Column &c) const
{
	const auto &p = m_params;
	float upper = densityUpperAt(c);
	const s32 floor = std::clamp(p.floor_y, -MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT);
	upper = std::fmin(std::fmax(upper, static_cast<float>(floor)),
		static_cast<float>(MAX_MAP_GENERATION_LIMIT));
	return static_cast<s32>(std::ceil(upper));
}

float ValleysBiomeTerrainSampler::densityAt(const TerrainPoint &point,
		const Column &c) const
{
	const auto &p = m_params;
	const auto [x, y, z] = point;
	float delta = static_cast<float>(y) - c.surface;
	float extent = std::fabs(c.slope) * p.fill_noise_magnitude;
	float density = -1.0f;
	if (delta < -extent)
		density = 1.0f;
	else if (delta <= extent)
		density = c.slope * NoiseFractal3D(&p.fill, x, y, z, p.seed) - delta;
	if (density <= 0.0f && c.mountain_gate > 0.0f && c.mountain_height > 0.0f &&
			static_cast<float>(y) > c.surface - 1.5f * std::fabs(c.slope)) {
		const float cap = p.mountain_cap != 0.0f ? capLift(delta, capAt(x, z)) : 0.0f;
		density = (NoiseFractal3D(&p.mountain, x, y, z, p.seed) + cap) *
			c.mountain_gate - delta / c.mountain_height;
	}
	return density;
}

bool ValleysBiomeTerrainSampler::naturalSolid(const TerrainPoint &point) const
{
	auto found = m_voxels.find(point);
	if (found != m_voxels.end())
		return found->second != AIR;
	const auto &p = m_params;
	const auto [x, y, z] = point;
	bool solid = y <= p.floor_y;
	if (!solid) {
		Column c = columnAt(x, z);
		solid = densityAt(point, c) > 0.0f;
	}
	if (m_voxels.size() < VOXEL_CACHE_LIMIT)
		m_voxels.emplace(point, solid ? SOLID : AIR);
	return solid;
}

float ValleysBiomeTerrainSampler::floaterFloor(const TerrainPoint &point) const
{
	Column c = columnAt(point.x, point.z);
	return c.surface - 1.5f * std::fabs(c.slope);
}

s32 ValleysBiomeTerrainSampler::initialTop(s32 x, s32 z, const TerrainChunk &chunk) const
{
	TerrainPoint key{x, chunk.min.y, z};
	auto found = m_tops.find(key);
	if (found != m_tops.end())
		return found->second;
	s32 y = std::min(upperAt(columnAt(x, z)), chunk.max.y);
	while (y >= chunk.min.y && !naturalSolid({x, y, z}))
		--y;
	if (m_tops.size() < TOP_CACHE_LIMIT)
		m_tops.emplace(key, y);
	return y;
}

bool ValleysBiomeTerrainSampler::retained(const TerrainPoint &point) const
{
	auto found = m_voxels.find(point);
	if (found != m_voxels.end()) {
		if (found->second == KEPT)
			return true;
		if (found->second == REMOVED)
			return false;
	}
	const auto chunk = chunkAt(point);
	const float floor = floaterFloor(point);
	if (static_cast<float>(point.y) <= floor || point.y <= m_params.floor_y)
		return true;

	// Most natural surfaces have a continuous vertical connection. Follow it
	// before considering a six-neighbour component search. It grounds the
	// piece only where the native fill would: at a solid node at or below
	// the floor, or at the bottom node of the chunk. The node that ends the
	// run is never assumed solid; if it is air, the piece may still float.
	s32 y = point.y;
	bool grounded = false;
	while (y >= chunk.min.y && naturalSolid({point.x, y, point.z})) {
		if (static_cast<float>(y) <= floor) {
			grounded = true;
			break;
		}
		--y;
	}
	if (y < chunk.min.y)
		grounded = true;
	if (grounded) {
		for (s32 yy = point.y; yy >= std::max(y, chunk.min.y); --yy)
			markVoxel({point.x, yy, point.z}, KEPT);
		return true;
	}

	static const TerrainPoint dirs[6] = {
		{1, 0, 0}, {-1, 0, 0}, {0, 0, 1},
		{0, 0, -1}, {0, 1, 0}, {0, -1, 0}
	};
	std::vector<TerrainPoint> stack{point};
	std::vector<TerrainPoint> piece;
	std::unordered_set<TerrainPoint, TerrainPointHash> visited{point};
	while (!stack.empty() && !grounded) {
		TerrainPoint current = stack.back();
		stack.pop_back();
		piece.push_back(current);
		if (piece.size() > COMPONENT_LIMIT ||
				static_cast<float>(current.y) <= floaterFloor(current) ||
				current.x == chunk.min.x || current.x == chunk.max.x ||
				current.y == chunk.min.y || current.y == chunk.max.y ||
				current.z == chunk.min.z || current.z == chunk.max.z) {
			grounded = true;
			break;
		}
		for (const auto &dir : dirs) {
			TerrainPoint next{current.x + dir.x, current.y + dir.y, current.z + dir.z};
			auto cached = m_voxels.find(next);
			if (cached != m_voxels.end() && cached->second == KEPT) {
				grounded = true;
				break;
			}
			if (visited.count(next) || !naturalSolid(next))
				continue;
			visited.insert(next);
			stack.push_back(next);
		}
	}
	bool seeded = false;
	if (!grounded) {
		// Native removal seeds only each column's original top in this Y
		// slab, and skips a top whose run of solid nodes reaches the floor
		// or the chunk bottom. An enclosed lower component with no such
		// seed must remain intact.
		std::unordered_map<u64, TerrainPoint> column_tops;
		for (const auto &voxel : piece) {
			u64 key = columnKey(voxel.x, voxel.z);
			auto top = column_tops.find(key);
			if (top == column_tops.end() || voxel.y > top->second.y)
				column_tops[key] = voxel;
		}
		for (const auto &entry : column_tops) {
			const auto &top = entry.second;
			if (initialTop(top.x, top.z, chunk) == top.y &&
					seedsRemoval(top, chunk)) {
				seeded = true;
				break;
			}
		}
	}
	const Voxel result = grounded || !seeded ? KEPT : REMOVED;
	for (const auto &voxel : visited)
		markVoxel(voxel, result);
	return result == KEPT;
}

// Whether the native fill starts from this column top: its topmost run of
// solid nodes has to end at a non-solid node above the floor and inside the
// chunk. A run that reaches the floor or the chunk bottom leaves the column
// alone without examining the node that ends it, even when that node is air
// and the piece floats; only a seed in another column can remove it then.
bool ValleysBiomeTerrainSampler::seedsRemoval(const TerrainPoint &top,
		const TerrainChunk &chunk) const
{
	const float floor = floaterFloor(top);
	if (static_cast<float>(top.y) <= floor)
		return false;
	s32 y = top.y;
	while (y >= chunk.min.y && static_cast<float>(y) > floor &&
			naturalSolid({top.x, y, top.z}))
		--y;
	return y >= chunk.min.y && static_cast<float>(y) > floor;
}

void ValleysBiomeTerrainSampler::markVoxel(const TerrainPoint &point, Voxel mark) const
{
	auto found = m_voxels.find(point);
	if (found != m_voxels.end())
		found->second = mark;
	else if (m_voxels.size() < VOXEL_CACHE_LIMIT)
		m_voxels.emplace(point, mark);
}

bool ValleysBiomeTerrainSampler::sampleClimate(v2s16 pos,
		BiomeClimateContext &out) const
{
	u64 key = columnKey(pos.X, pos.Y);
	auto found = m_climates.find(key);
	if (found != m_climates.end()) {
		out = found->second;
		return true;
	}

	trimColumnCaches();
	const Column c = columnAt(pos.X, pos.Y);
	if (!std::isfinite(c.surface) || !std::isfinite(c.bank) ||
			!std::isfinite(c.form.base) || !std::isfinite(c.form.valley_depth) ||
			!std::isfinite(c.form.valley_pos) || !std::isfinite(c.form.mountain))
		return false;
	BiomeTerrainForm form = c.form;
	if (c.mountain_gate > 0.0f && c.mountain_height > 0.0f)
		form.body = bodyHeightAt(pos.X, pos.Y, c);
	if (!std::isfinite(form.body))
		return false;

	// Clamp before conversion, preserving truncation toward zero even for
	// negative fractional surfaces. The 2D surface is the climate height:
	// 3D relief and mountain bodies above it do not cool the column, and
	// the form fields carry where the column sits instead.
	const float low = -MAX_MAP_GENERATION_LIMIT;
	const float high = MAX_MAP_GENERATION_LIMIT;
	BiomeClimateContext context{c.bank,
		static_cast<s16>(std::clamp(c.surface, low, high)), form};

	if (m_climates.size() >= CLIMATE_CACHE_LIMIT)
		m_climates.clear();
	m_climates.emplace(key, context);
	out = context;
	return true;
}

float ValleysBiomeTerrainSampler::modelHeight(s32 x, s32 z) const
{
	trimSurfaceCaches();
	const s32 floor = std::clamp(m_params.floor_y,
		-MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT);
	const s32 top = upperAt(columnAt(x, z));
	for (s32 y = top; y > floor; --y) {
		TerrainPoint point{x, y, z};
		if (naturalSolid(point) && (!m_params.remove_floaters || retained(point)))
			return static_cast<float>(y);
	}
	return static_cast<float>(floor);
}

float ValleysBiomeTerrainSampler::sampleHeight(v2s16 pos) const
{
	u64 key = columnKey(pos.X, pos.Y);
	auto found = m_heights.find(key);
	if (found != m_heights.end())
		return found->second;
	float height = modelHeight(pos.X, pos.Y);
	if (m_heights.size() >= HEIGHT_CACHE_LIMIT)
		m_heights.clear();
	m_heights.emplace(key, height);
	return height;
}

} // namespace

std::unique_ptr<BiomeTerrainSampler> createValleysBiomeTerrainSampler(
		const MapgenValleysParams &params)
{
	return std::make_unique<ValleysBiomeTerrainSampler>(params);
}

#endif
