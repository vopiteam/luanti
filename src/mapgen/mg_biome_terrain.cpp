// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mg_biome_terrain.h"

#if IS_VOPI_ENGINE

#include "mapgen_valleys.h"
#include "constants.h"
#include "noise.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace {

constexpr s32 SAMPLE_STEP = 8;
constexpr s32 RELIEF_RADIUS = 16;
constexpr size_t HEIGHT_CACHE_LIMIT = 4096;
constexpr size_t METRIC_CACHE_LIMIT = 2048;
constexpr size_t COLUMN_CACHE_LIMIT = 32768;
constexpr size_t CAP_CACHE_LIMIT = 4096;
constexpr size_t VOXEL_CACHE_LIMIT = 262144;
constexpr size_t TOP_CACHE_LIMIT = 16384;
constexpr size_t COMPONENT_LIMIT = 65536;
constexpr float RAD_TO_DEG = 57.29577951308232f;

u64 columnKey(s32 x, s32 z)
{
	return (static_cast<u64>(static_cast<u32>(x)) << 32) |
		static_cast<u32>(z);
}

s32 latticeFloor(s32 value)
{
	s32 remainder = value % SAMPLE_STEP;
	if (remainder < 0)
		remainder += SAMPLE_STEP;
	return value - remainder;
}

float interpolate(float a, float b, float c, float d, float x, float z)
{
	return (a + (b - a) * x) * (1.0f - z) + (c + (d - c) * x) * z;
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
	float water_level;
	float river_size_factor;
	float river_depth_bed;
	float river_valley_width;
	float river_bank_height;
	s32 floor_y;
	bool sea_level_rivers;
	bool mountains;
	bool carve_cliffs;
	bool remove_floaters;
	TerrainPoint chunk_blocks;
	float carve_zero_height;
	s32 carve_reach;
	float carve_undercut;
	float mountain_river_width;
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
	NoiseParams carve;

	explicit ValleysTerrainParams(const MapgenValleysParams &p) :
		seed(static_cast<s32>(p.seed)),
		water_level(p.water_level),
		river_size_factor(p.river_size / 100.0f),
		river_depth_bed(p.river_depth + 1.0f),
		river_valley_width(p.river_valley_width),
		river_bank_height(p.river_bank_height),
		floor_y(p.floor_y),
		sea_level_rivers(p.spflags & MGVALLEYS_SEA_LEVEL_RIVERS),
		mountains(p.spflags & MGVALLEYS_MOUNTAINS),
		carve_cliffs(p.spflags & MGVALLEYS_CARVE_CLIFFS),
		remove_floaters(p.spflags & MGVALLEYS_REMOVE_FLOATERS),
		chunk_blocks{std::max<s32>(p.chunksize.X, 1),
			std::max<s32>(p.chunksize.Y, 1), std::max<s32>(p.chunksize.Z, 1)},
		carve_zero_height(std::fmax(static_cast<float>(p.carve_zero_height), 1.0f)),
		carve_reach(static_cast<s16>(p.carve_reach)),
		carve_undercut(p.carve_undercut),
		mountain_river_width(std::fmax(p.mountain_river_width, 0.01f)),
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
		carve(p.np_carve)
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

class ValleysBiomeTerrainSampler final : public HeightmapBiomeTerrainSampler {
public:
	explicit ValleysBiomeTerrainSampler(const MapgenValleysParams &params) :
		m_params(params)
	{
	}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::unique_ptr<BiomeTerrainSampler>(new ValleysBiomeTerrainSampler(m_params));
	}

	void resetCache() override
	{
		HeightmapBiomeTerrainSampler::resetCache();
		m_columns.clear();
		m_caps.clear();
		m_carves.clear();
		m_voxels.clear();
		m_tops.clear();
	}

protected:
	float sampleHeight(s32 x, s32 z) const override;

private:
	explicit ValleysBiomeTerrainSampler(const ValleysTerrainParams &params) :
		m_params(params)
	{
	}

	struct Column {
		float surface;
		float bank;
		float slope;
		float mountain_gate;
		float mountain_height;
		float foot;
	};
	struct Carve {
		float foot;
		float height;
		float gate;
		s32 ymin;
		s32 ymax;
	};
	enum Voxel : u8 { AIR, SOLID, KEPT, REMOVED };
	Column columnAt(s32 x, s32 z) const;
	Carve carveAt(s32 x, s32 z) const;
	TerrainChunk chunkAt(const TerrainPoint &point) const;
	s32 upperAt(const Column &column) const;
	bool naturalSolid(const TerrainPoint &point) const;
	float floaterFloor(const TerrainPoint &point, const TerrainChunk &chunk) const;
	s32 initialTop(s32 x, s32 z, const TerrainChunk &chunk) const;
	bool retained(const TerrainPoint &point) const;
	bool seedsRemoval(const TerrainPoint &top, const TerrainChunk &chunk) const;
	void markVoxel(const TerrainPoint &point, Voxel mark) const;
	void trimSurfaceCaches() const;
	float capAt(s32 x, s32 z) const;
	const ValleysTerrainParams m_params;
	mutable std::unordered_map<u64, Column> m_columns;
	mutable std::unordered_map<u64, float> m_caps;
	mutable std::unordered_map<u64, Carve> m_carves;
	mutable std::unordered_map<TerrainPoint, Voxel, TerrainPointHash> m_voxels;
	mutable std::unordered_map<TerrainPoint, s32, TerrainPointHash> m_tops;
};

ValleysBiomeTerrainSampler::Column ValleysBiomeTerrainSampler::columnAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_columns.find(key);
	if (found != m_columns.end())
		return found->second;

	const auto &p = m_params;
	float n_slope = NoiseFractal2D(&p.slope, x, z, p.seed);
	float n_rivers = NoiseFractal2D(&p.rivers, x, z, p.seed);
	float n_terrain = NoiseFractal2D(&p.terrain_height, x, z, p.seed);
	float n_valley = NoiseFractal2D(&p.valley_depth, x, z, p.seed);
	float n_profile = NoiseFractal2D(&p.valley_profile, x, z, p.seed);
	float valley_d = n_valley * n_valley;
	float base = n_terrain + valley_d;
	float river = std::fabs(n_rivers) - p.river_size_factor;
	float tv = std::fmax(river / n_profile, 0.0f);
	float valley_h = valley_d * (1.0f - std::exp(-tv * tv));
	float surface = base + valley_h;
	float slope = n_slope * valley_h;
	bool river_water = false;

	// Keep this terrain shape calculation in step with terrainColumn(). No
	// node data, climate adjustments or existing mapgen noise buffers change.
	if (p.sea_level_rivers) {
		float bank = p.water_level + p.river_bank_height;
		if (base > bank) {
			float tg = std::fmax(river / (n_profile * p.river_valley_width), 0.0f);
			surface -= (base - bank) * std::exp(-tg * tg);
			base = bank;
			river_water = river < 0.0f;
			slope = std::fmin(slope, n_slope * (surface - base));
		}
	}
	if (river < 0.0f) {
		float tr = river / p.river_size_factor + 1.0f;
		float depth = p.river_depth_bed * std::sqrt(std::fmax(0.0f, 1.0f - tr * tr));
		surface = std::fmin(std::fmax(base - depth, p.water_level - 3.0f), surface);
		slope = 0.0f;
		if (river_water)
			surface = std::fmin(base - depth, surface);
	}

	Column c{surface, base, slope, 0.0f, 0.0f, 0.0f};
	if (p.mountains) {
		c.mountain_height = NoiseFractal2D(&p.mountain_height, x, z, p.seed);
		if (c.mountain_height > 0.0f) {
			float tm = std::fmax(river / (n_profile * p.mountain_river_width), 0.0f);
			c.mountain_gate = 1.0f - std::exp(-tm * tm);
			if (c.mountain_gate > 0.0f) {
				float ys = std::floor(surface + 0.5f);
				c.foot = std::fmax(NoiseFractal3D(&p.mountain, x, ys, z, p.seed) *
					c.mountain_gate, 0.0f);
			}
		}
	}
	if (m_columns.size() < COLUMN_CACHE_LIMIT)
		m_columns.emplace(key, c);
	return c;
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
		float row = 0.0f;
		for (s32 dx = -reach; dx <= reach; ++dx)
			row = std::fmax(row, columnAt(x + dx, z + dz).foot *
				(1.0f - static_cast<float>(std::abs(dx)) * taper));
		foot = std::fmax(foot, row * (1.0f - static_cast<float>(std::abs(dz)) * taper));
	}
	if (m_caps.size() < CAP_CACHE_LIMIT)
		m_caps.emplace(key, foot);
	return foot;
}

void ValleysBiomeTerrainSampler::trimSurfaceCaches() const
{
	// Never evict while exploring a connected component. Insertion budgets
	// also bound memory for a single unusually expensive height query.
	if (m_columns.size() >= COLUMN_CACHE_LIMIT)
		m_columns.clear();
	if (m_caps.size() >= CAP_CACHE_LIMIT)
		m_caps.clear();
	if (m_carves.size() >= CAP_CACHE_LIMIT)
		m_carves.clear();
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

ValleysBiomeTerrainSampler::Carve ValleysBiomeTerrainSampler::carveAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_carves.find(key);
	if (found != m_carves.end())
		return found->second;
	Column c = columnAt(x, z);
	float foot = c.surface;
	const auto &p = m_params;
	const auto chunk = chunkAt({x, 0, z});
	// Native carving clamps the local minimum to this canonical chunk. An
	// unrestricted halo would change the cliff near its X/Z boundaries.
	for (s32 zz = std::max(chunk.min.z, z - p.carve_reach);
			zz <= std::min(chunk.max.z, z + p.carve_reach); ++zz)
	for (s32 xx = std::max(chunk.min.x, x - p.carve_reach);
			xx <= std::min(chunk.max.x, x + p.carve_reach); ++xx)
		foot = std::fmin(foot, columnAt(xx, zz).surface);
	Carve carve{foot, c.surface - foot,
		std::clamp((c.surface - c.bank) / p.carve_zero_height, 0.0f, 1.0f), 1, 0};
	if (carve.height >= 2.0f && carve.gate > 0.0f) {
		// Keep inactive or wholly out-of-range walls empty. Clip in double:
		// float(s32::max) rounds above the largest representable integer.
		const double lowest = std::numeric_limits<s32>::min();
		const double highest = std::numeric_limits<s32>::max();
		double ymin = std::max(std::floor(static_cast<double>(foot + 1.0f)),
			static_cast<double>(p.water_level) + 1.0);
		double ymax = std::floor(static_cast<double>(c.surface));
		if (ymin <= highest && ymax >= lowest && ymin <= ymax) {
			carve.ymin = static_cast<s32>(std::max(ymin, lowest));
			carve.ymax = static_cast<s32>(std::min(ymax, highest));
		}
	}
	if (m_carves.size() < CAP_CACHE_LIMIT)
		m_carves.emplace(key, carve);
	return carve;
}

s32 ValleysBiomeTerrainSampler::upperAt(const Column &c) const
{
	const auto &p = m_params;
	float upper = c.surface + std::fabs(c.slope) * p.fill_noise_magnitude;
	if (c.mountain_gate > 0.0f && c.mountain_height > 0.0f)
		upper = std::fmax(upper, c.surface + std::fmax(p.mountain_cap_height,
			p.mountain_noise_magnitude * c.mountain_gate * c.mountain_height));
	const s32 floor = std::clamp(p.floor_y, -MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT);
	upper = std::fmin(std::fmax(upper, static_cast<float>(floor)),
		static_cast<float>(MAX_MAP_GENERATION_LIMIT));
	return static_cast<s32>(std::ceil(upper));
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
		float delta = static_cast<float>(y) - c.surface;
		float extent = std::fabs(c.slope) * p.fill_noise_magnitude;
		float density = -1.0f;
		if (delta < -extent)
			density = 1.0f;
		else if (delta <= extent)
			density = c.slope * NoiseFractal3D(&p.fill, x, y, z, p.seed) - delta;
		if (density <= 0.0f && c.mountain_gate > 0.0f && c.mountain_height > 0.0f &&
				static_cast<float>(y) > c.surface - 1.5f * std::fabs(c.slope)) {
			float cap = 0.0f;
			if (delta > 0.0f && delta < p.mountain_cap_height && p.mountain_cap != 0.0f) {
				float t = delta / p.mountain_cap_height;
				cap = p.mountain_cap * capAt(x, z) * 4.0f * t * (1.0f - t);
			}
			density = (NoiseFractal3D(&p.mountain, x, y, z, p.seed) + cap) *
				c.mountain_gate - delta / c.mountain_height;
		}
		solid = density > 0.0f;
		if (solid && p.carve_cliffs) {
			Carve carve = carveAt(x, z);
			if (carve.height >= 2.0f && carve.gate > 0.0f &&
					y >= carve.ymin && y <= carve.ymax) {
				float t = (c.surface - static_cast<float>(y)) / carve.height;
				solid = NoiseFractal3D(&p.carve, x, y, z, p.seed) * carve.gate +
					p.carve_undercut * t <= 0.0f;
			}
		}
	}
	if (m_voxels.size() < VOXEL_CACHE_LIMIT)
		m_voxels.emplace(point, solid ? SOLID : AIR);
	return solid;
}

float ValleysBiomeTerrainSampler::floaterFloor(const TerrainPoint &point,
		const TerrainChunk &chunk) const
{
	Column c = columnAt(point.x, point.z);
	float floor = c.surface - 1.5f * std::fabs(c.slope);
	if (m_params.carve_cliffs) {
		Carve carve = carveAt(point.x, point.z);
		if (carve.height >= 2.0f && carve.gate > 0.0f &&
				std::max(carve.ymin, chunk.min.y) <= std::min(carve.ymax, chunk.max.y))
			floor = std::fmin(floor, carve.foot);
	}
	return floor;
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
	const float floor = floaterFloor(point, chunk);
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
				static_cast<float>(current.y) <= floaterFloor(current, chunk) ||
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
	const float floor = floaterFloor(top, chunk);
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

float ValleysBiomeTerrainSampler::sampleHeight(s32 x, s32 z) const
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

} // namespace

float HeightmapBiomeTerrainSampler::heightAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_heights.find(key);
	if (found != m_heights.end())
		return found->second;
	float height = sampleHeight(x, z);
	if (m_heights.size() >= HEIGHT_CACHE_LIMIT)
		m_heights.clear();
	m_heights.emplace(key, height);
	return height;
}

BiomeTerrain HeightmapBiomeTerrainSampler::latticeAt(s32 x, s32 z) const
{
	u64 key = columnKey(x, z);
	auto found = m_metrics.find(key);
	if (found != m_metrics.end())
		return found->second;
	float low = std::numeric_limits<float>::max();
	float high = std::numeric_limits<float>::lowest();
	for (s32 dz = -RELIEF_RADIUS; dz <= RELIEF_RADIUS; dz += SAMPLE_STEP)
	for (s32 dx = -RELIEF_RADIUS; dx <= RELIEF_RADIUS; dx += SAMPLE_STEP) {
		float height = heightAt(x + dx, z + dz);
		low = std::fmin(low, height);
		high = std::fmax(high, height);
	}
	const float center = heightAt(x, z);
	// Opposite flanks must not cancel at a ridge or a valley. One-sided
	// axis maxima preserve the exact gradient of a plane; the diagonals
	// also cover steep features that neither axis intersects.
	float dx = std::fmax(std::fabs(heightAt(x + SAMPLE_STEP, z) - center),
		std::fabs(heightAt(x - SAMPLE_STEP, z) - center)) / SAMPLE_STEP;
	float dz = std::fmax(std::fabs(heightAt(x, z + SAMPLE_STEP) - center),
		std::fabs(heightAt(x, z - SAMPLE_STEP) - center)) / SAMPLE_STEP;
	float gradient = std::hypot(dx, dz);
	const float diagonal_distance = SAMPLE_STEP * std::sqrt(2.0f);
	for (s32 oz : {-SAMPLE_STEP, SAMPLE_STEP})
	for (s32 ox : {-SAMPLE_STEP, SAMPLE_STEP})
		gradient = std::fmax(gradient,
			std::fabs(heightAt(x + ox, z + oz) - center) / diagonal_distance);
	BiomeTerrain result{center, std::atan(gradient) * RAD_TO_DEG, high - low};
	if (m_metrics.size() >= METRIC_CACHE_LIMIT)
		m_metrics.clear();
	m_metrics.emplace(key, result);
	return result;
}

BiomeTerrain HeightmapBiomeTerrainSampler::sample(v2s16 pos) const
{
	s32 x = latticeFloor(pos.X);
	s32 z = latticeFloor(pos.Y);
	float tx = static_cast<float>(pos.X - x) / SAMPLE_STEP;
	float tz = static_cast<float>(pos.Y - z) / SAMPLE_STEP;
	BiomeTerrain a = latticeAt(x, z);
	BiomeTerrain b = latticeAt(x + SAMPLE_STEP, z);
	BiomeTerrain c = latticeAt(x, z + SAMPLE_STEP);
	BiomeTerrain d = latticeAt(x + SAMPLE_STEP, z + SAMPLE_STEP);
	return {
		interpolate(a.height, b.height, c.height, d.height, tx, tz),
		interpolate(a.slope, b.slope, c.slope, d.slope, tx, tz),
		interpolate(a.relief, b.relief, c.relief, d.relief, tx, tz)
	};
}

void HeightmapBiomeTerrainSampler::resetCache()
{
	m_heights.clear();
	m_metrics.clear();
}

std::unique_ptr<BiomeTerrainSampler> createValleysBiomeTerrainSampler(
		const MapgenValleysParams &params)
{
	return std::make_unique<ValleysBiomeTerrainSampler>(params);
}

#endif
