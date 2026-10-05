/*
Luanti
SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (C) 2016-2019 Duane Robertson <duane@duanerobertson.com>
Copyright (C) 2016-2019 paramat

Based on Valleys Mapgen by Gael de Sailly
(https://forum.luanti.org/viewtopic.php?f=9&t=11430)
and mapgen_v7, mapgen_flat by kwolekr and paramat.

Licensing changed by permission of Gael de Sailly.
*/


#include "mapgen.h"
#include "voxel.h"
#include "noise.h"
#include "mapnode.h"
#include "map.h"
#include "nodedef.h"
#include "voxelalgorithms.h"
#include "profiler.h"
#include "porting.h"
#include "settings.h" // For g_settings
#include "emerge.h"
#include "dungeongen.h"
#include "mg_biome.h"
#include "mg_ore.h"
#include "mg_decoration.h"
#include "mapgen_valleys.h"
#include "cavegen.h"
#include <cmath>
#if IS_VOPI_ENGINE
#include <algorithm>
#include "exceptions.h"
#endif


const FlagDesc flagdesc_mapgen_valleys[] = {
	{"altitude_chill",   MGVALLEYS_ALT_CHILL},
	{"humid_rivers",     MGVALLEYS_HUMID_RIVERS},
	{"vary_river_depth", MGVALLEYS_VARY_RIVER_DEPTH},
	{"altitude_dry",     MGVALLEYS_ALT_DRY},
#if IS_VOPI_ENGINE
	{"sea_level_rivers", MGVALLEYS_SEA_LEVEL_RIVERS},
	{"remove_floaters",  MGVALLEYS_REMOVE_FLOATERS},
	{"mountains",        MGVALLEYS_MOUNTAINS},
	{"wetlands",         MGVALLEYS_WETLANDS},
#endif
	{NULL,               0}
};

#if IS_VOPI_ENGINE
namespace {
float clampAltitudeChill(float value)
{
	// This distance is a divisor in both climate and river-depth calculations.
	return std::fmax(value, 1.0f);
}
}

ValleysClimate calcValleysClimate(float heat, float humidity,
		float base, s16 column_max_y, int water_level, float altitude_chill, u32 flags)
{
	altitude_chill = clampAltitudeChill(altitude_chill);

	// Optionally increase humidity around rivers
	if (flags & MGVALLEYS_HUMID_RIVERS) {
		// Compensate to avoid increasing average humidity
		humidity *= 0.8f;
		// Ground height ignoring riverbeds
		float t_alt = std::fmax(base, (float)column_max_y);
		float water_depth = (t_alt - base) / 4.0f;
		humidity *= 1.0f + std::pow(0.5f, std::fmax(water_depth, 1.0f));
	}

	// Optionally decrease humidity with altitude
	if (flags & MGVALLEYS_ALT_DRY) {
		// Ground height ignoring riverbeds
		float t_alt = std::fmax(base, (float)column_max_y);
		// Only decrease above water_level
		if (t_alt > water_level)
			humidity -= (t_alt - water_level) * 10.0f / altitude_chill;
	}

	// Optionally decrease heat with altitude
	if (flags & MGVALLEYS_ALT_CHILL) {
		// Compensate to avoid reducing the average heat
		heat += 5.0f;
		// Ground height ignoring riverbeds
		float t_alt = std::fmax(base, (float)column_max_y);
		// Only decrease above water_level
		if (t_alt > water_level)
			heat -= (t_alt - water_level) * 20.0f / altitude_chill;
	}

	return {heat, humidity};
}
#endif


MapgenValleys::MapgenValleys(MapgenValleysParams *params, EmergeParams *emerge)
	: MapgenBasic(MAPGEN_VALLEYS, params, emerge)
#if IS_VOPI_ENGINE
	, column_params(*params)
#endif
{
	FATAL_ERROR_IF(biomegen->getType() != BIOMEGEN_ORIGINAL,
		"MapgenValleys has a hard dependency on BiomeGenOriginal");
	m_bgen = (BiomeGenOriginal *)biomegen;

	spflags            = params->spflags;
#if IS_VOPI_ENGINE
	altitude_chill     = clampAltitudeChill(params->altitude_chill);
	// The emerge manager gives the source generator the column model of
	// these same parameters and every worker is its clone; a generator made
	// without one, as the tests do, gets it here
	if (!m_bgen->hasEffectiveClimate())
		m_bgen->setValleysClimate(*params);
#else
	altitude_chill     = params->altitude_chill;
#endif
	river_depth_bed    = params->river_depth + 1.0f;
	river_size_factor  = params->river_size / 100.0f;
#if IS_VOPI_ENGINE
	river_valley_width = params->river_valley_width;
	river_bank_height  = params->river_bank_height;
	floor_y            = params->floor_y;
	mountain_river_width = column_params.mountain_river_width;
	mountain_cap        = params->mountain_cap;
	mountain_cap_height = std::fmax((float)params->mountain_cap_height, 1.0f);
	// The 2D noises grow with the square of the reach
	mountain_cap_reach  = (s16)std::min<u16>(params->mountain_cap_reach, 32);
	mountain_noise_max  = 0.0f;
	if (spflags & MGVALLEYS_MOUNTAINS)
		column_reach = mountain_cap_reach;
	for (size_t i = 0; i < m_bmgr->getNumObjects(); i++) {
		const Biome *b = (const Biome *)m_bmgr->getRaw(i);
		if (!b)
			continue;
		for (content_t c : {b->c_water_top, b->c_water, b->c_river_water}) {
			if (c != CONTENT_IGNORE && ndef->get(c).walkable &&
					!CONTAINS(water_lids, c))
				water_lids.push_back(c);
		}
	}
#endif

	cave_width         = params->cave_width;
	large_cave_depth   = params->large_cave_depth;
	small_cave_num_min = params->small_cave_num_min;
	small_cave_num_max = params->small_cave_num_max;
	large_cave_num_min = params->large_cave_num_min;
	large_cave_num_max = params->large_cave_num_max;
	large_cave_flooded = params->large_cave_flooded;
	cavern_limit       = params->cavern_limit;
	cavern_taper       = params->cavern_taper;
	cavern_threshold   = params->cavern_threshold;
	dungeon_ymin       = params->dungeon_ymin;
	dungeon_ymax       = params->dungeon_ymax;

	//// 2D Terrain noise
	// The terrain noises cover 'column_reach' beyond the mapchunk on every
	// side, the filler depth only the mapchunk
	const u32 area_x = csize.X + 2 * column_reach;
	const u32 area_z = csize.Z + 2 * column_reach;
	noise_filler_depth       = new Noise(&params->np_filler_depth,       seed, csize.X, csize.Z);
	noise_inter_valley_slope = new Noise(&params->np_inter_valley_slope, seed, area_x, area_z);
	noise_rivers             = new Noise(&params->np_rivers,             seed, area_x, area_z);
	noise_terrain_height     = new Noise(&params->np_terrain_height,     seed, area_x, area_z);
	noise_valley_depth       = new Noise(&params->np_valley_depth,       seed, area_x, area_z);
	noise_valley_profile     = new Noise(&params->np_valley_profile,     seed, area_x, area_z);

	//// 3D Terrain noise
	// 1-up 1-down overgeneration
	noise_inter_valley_fill = new Noise(&params->np_inter_valley_fill,
		seed, csize.X, csize.Y + 2, csize.Z);
#if IS_VOPI_ENGINE
	if (spflags & MGVALLEYS_MOUNTAINS) {
		// 2D noise
		noise_mountain_height = new Noise(&params->np_mountain_height,
			seed, area_x, area_z);
		// 3D noise, 1-up 1-down overgeneration
		noise_mountain = new Noise(&params->np_mountain,
			seed, csize.X, csize.Y + 2, csize.Z);
		// The octaves at their largest, then offset and scale
		const NoiseParams &np = params->np_mountain;
		float octaves_max = (np.persist == 1.0f) ? (float)np.octaves :
			(1.0f - std::pow(np.persist, (float)np.octaves)) /
			(1.0f - np.persist);
		mountain_noise_max = np.offset + std::fabs(np.scale) * octaves_max;
	}
	if (spflags & MGVALLEYS_WETLANDS) {
		// 2D noise
		noise_wetland_pools = new Noise(&params->np_wetland_pools,
			seed, area_x, area_z);
	}
	floater_floor.resize((size_t)csize.X * csize.Z);
#endif
	// 1-down overgeneraion
	MapgenBasic::np_cave1    = params->np_cave1;
	MapgenBasic::np_cave2    = params->np_cave2;
	MapgenBasic::np_cavern   = params->np_cavern;
	MapgenBasic::np_dungeons = params->np_dungeons;
}


MapgenValleys::~MapgenValleys()
{
	delete noise_filler_depth;
	delete noise_inter_valley_fill;
	delete noise_inter_valley_slope;
	delete noise_rivers;
	delete noise_terrain_height;
	delete noise_valley_depth;
	delete noise_valley_profile;
#if IS_VOPI_ENGINE
	delete noise_mountain;
	delete noise_mountain_height;
	delete noise_wetland_pools;
#endif
}


MapgenValleysParams::MapgenValleysParams():
	np_filler_depth       (0.0,   1.2,  v3f(256,  256,  256),  1605,  3, 0.5,  2.0),
	np_inter_valley_fill  (0.0,   1.0,  v3f(256,  512,  256),  1993,  6, 0.8,  2.0),
	np_inter_valley_slope (0.5,   0.5,  v3f(128,  128,  128),  746,   1, 1.0,  2.0),
	np_rivers             (0.0,   1.0,  v3f(256,  256,  256),  -6050, 5, 0.6,  2.0),
	np_terrain_height     (-10.0, 50.0, v3f(1024, 1024, 1024), 5202,  6, 0.4,  2.0),
	np_valley_depth       (5.0,   4.0,  v3f(512,  512,  512),  -1914, 1, 1.0,  2.0),
	np_valley_profile     (0.6,   0.50, v3f(512,  512,  512),  777,   1, 1.0,  2.0),
	np_cave1              (0.0,   12.0, v3f(61,   61,   61),   52534, 3, 0.5,  2.0),
	np_cave2              (0.0,   12.0, v3f(67,   67,   67),   10325, 3, 0.5,  2.0),
	np_cavern             (0.0,   1.0,  v3f(768,  256,  768),  59033, 6, 0.63, 2.0),
	np_dungeons           (0.9,   0.5,  v3f(500,  500,  500),  0,     2, 0.8,  2.0)
#if IS_VOPI_ENGINE
	, np_mountain         (-0.48, 1.0,  v3f(192,  256,  192),  3517,  5, 0.7,  2.0)
	, np_mountain_height  (128.0, 80.0, v3f(1500, 1500, 1500), 4021,  3, 0.6,  2.0)
	, np_wetland_pools    (-0.4,  2.0,  v3f(40,   40,   40),   8125,  4, 0.55, 2.0)
#endif
{
}


void MapgenValleysParams::readParams(const Settings *settings)
{
	settings->getFlagStrNoEx("mgvalleys_spflags", spflags, flagdesc_mapgen_valleys);
	settings->getU16NoEx("mgvalleys_altitude_chill",       altitude_chill);
#if IS_VOPI_ENGINE
	altitude_chill = clampAltitudeChill(altitude_chill);
#endif
	settings->getS16NoEx("mgvalleys_large_cave_depth",     large_cave_depth);
	settings->getU16NoEx("mgvalleys_small_cave_num_min",   small_cave_num_min);
	settings->getU16NoEx("mgvalleys_small_cave_num_max",   small_cave_num_max);
	settings->getU16NoEx("mgvalleys_large_cave_num_min",   large_cave_num_min);
	settings->getU16NoEx("mgvalleys_large_cave_num_max",   large_cave_num_max);
	settings->getFloatNoEx("mgvalleys_large_cave_flooded", large_cave_flooded);
	settings->getU16NoEx("mgvalleys_river_depth",          river_depth);
	settings->getU16NoEx("mgvalleys_river_size",           river_size);
#if IS_VOPI_ENGINE
	settings->getFloatNoEx("mgvalleys_river_valley_width", river_valley_width);
	settings->getU16NoEx("mgvalleys_river_bank_height",    river_bank_height);
	settings->getS16NoEx("mgvalleys_floor_y",              floor_y);
	settings->getFloatNoEx("mgvalleys_mountain_river_width", mountain_river_width);
	settings->getFloatNoEx("mgvalleys_mountain_cap",         mountain_cap);
	settings->getU16NoEx("mgvalleys_mountain_cap_height",    mountain_cap_height);
	settings->getU16NoEx("mgvalleys_mountain_cap_reach",     mountain_cap_reach);
	settings->getFloatNoEx("mgvalleys_wetland_base_min",     wetland_base_min);
	settings->getFloatNoEx("mgvalleys_wetland_base_max",     wetland_base_max);
	settings->getFloatNoEx("mgvalleys_wetland_valley_depth_max", wetland_valley_depth_max);
	settings->getFloatNoEx("mgvalleys_wetland_fade",         wetland_fade);
	settings->getU16NoEx("mgvalleys_wetland_height",         wetland_height);
	settings->getU16NoEx("mgvalleys_wetland_pool_depth",     wetland_pool_depth);
	settings->getFloatNoEx("mgvalleys_wetland_pool_edge",    wetland_pool_edge);
#endif
	settings->getFloatNoEx("mgvalleys_cave_width",         cave_width);
	settings->getS16NoEx("mgvalleys_cavern_limit",         cavern_limit);
	settings->getS16NoEx("mgvalleys_cavern_taper",         cavern_taper);
	settings->getFloatNoEx("mgvalleys_cavern_threshold",   cavern_threshold);
	settings->getS16NoEx("mgvalleys_dungeon_ymin",         dungeon_ymin);
	settings->getS16NoEx("mgvalleys_dungeon_ymax",         dungeon_ymax);

	settings->getNoiseParams("mgvalleys_np_filler_depth",       np_filler_depth);
	settings->getNoiseParams("mgvalleys_np_inter_valley_fill",  np_inter_valley_fill);
	settings->getNoiseParams("mgvalleys_np_inter_valley_slope", np_inter_valley_slope);
	settings->getNoiseParams("mgvalleys_np_rivers",             np_rivers);
	settings->getNoiseParams("mgvalleys_np_terrain_height",     np_terrain_height);
	settings->getNoiseParams("mgvalleys_np_valley_depth",       np_valley_depth);
	settings->getNoiseParams("mgvalleys_np_valley_profile",     np_valley_profile);

	settings->getNoiseParams("mgvalleys_np_cave1",              np_cave1);
	settings->getNoiseParams("mgvalleys_np_cave2",              np_cave2);
	settings->getNoiseParams("mgvalleys_np_cavern",             np_cavern);
	settings->getNoiseParams("mgvalleys_np_dungeons",           np_dungeons);
#if IS_VOPI_ENGINE
	settings->getNoiseParams("mgvalleys_np_mountain",           np_mountain);
	settings->getNoiseParams("mgvalleys_np_mountain_height",    np_mountain_height);
	settings->getNoiseParams("mgvalleys_np_wetland_pools",      np_wetland_pools);
#endif
}


void MapgenValleysParams::writeParams(Settings *settings) const
{
	settings->setFlagStr("mgvalleys_spflags", spflags, flagdesc_mapgen_valleys);
#if IS_VOPI_ENGINE
	settings->setU16("mgvalleys_altitude_chill", clampAltitudeChill(altitude_chill));
#else
	settings->setU16("mgvalleys_altitude_chill",       altitude_chill);
#endif
	settings->setS16("mgvalleys_large_cave_depth",     large_cave_depth);
	settings->setU16("mgvalleys_small_cave_num_min",   small_cave_num_min);
	settings->setU16("mgvalleys_small_cave_num_max",   small_cave_num_max);
	settings->setU16("mgvalleys_large_cave_num_min",   large_cave_num_min);
	settings->setU16("mgvalleys_large_cave_num_max",   large_cave_num_max);
	settings->setFloat("mgvalleys_large_cave_flooded", large_cave_flooded);
	settings->setU16("mgvalleys_river_depth",          river_depth);
	settings->setU16("mgvalleys_river_size",           river_size);
#if IS_VOPI_ENGINE
	settings->setFloat("mgvalleys_river_valley_width", river_valley_width);
	settings->setU16("mgvalleys_river_bank_height",    river_bank_height);
	settings->setS16("mgvalleys_floor_y",              floor_y);
	settings->setFloat("mgvalleys_mountain_river_width", mountain_river_width);
	settings->setFloat("mgvalleys_mountain_cap",         mountain_cap);
	settings->setU16("mgvalleys_mountain_cap_height",    mountain_cap_height);
	settings->setU16("mgvalleys_mountain_cap_reach",     mountain_cap_reach);
	settings->setFloat("mgvalleys_wetland_base_min",     wetland_base_min);
	settings->setFloat("mgvalleys_wetland_base_max",     wetland_base_max);
	settings->setFloat("mgvalleys_wetland_valley_depth_max", wetland_valley_depth_max);
	settings->setFloat("mgvalleys_wetland_fade",         wetland_fade);
	settings->setU16("mgvalleys_wetland_height",         wetland_height);
	settings->setU16("mgvalleys_wetland_pool_depth",     wetland_pool_depth);
	settings->setFloat("mgvalleys_wetland_pool_edge",    wetland_pool_edge);
#endif
	settings->setFloat("mgvalleys_cave_width",         cave_width);
	settings->setS16("mgvalleys_cavern_limit",         cavern_limit);
	settings->setS16("mgvalleys_cavern_taper",         cavern_taper);
	settings->setFloat("mgvalleys_cavern_threshold",   cavern_threshold);
	settings->setS16("mgvalleys_dungeon_ymin",         dungeon_ymin);
	settings->setS16("mgvalleys_dungeon_ymax",         dungeon_ymax);

	settings->setNoiseParams("mgvalleys_np_filler_depth",       np_filler_depth);
	settings->setNoiseParams("mgvalleys_np_inter_valley_fill",  np_inter_valley_fill);
	settings->setNoiseParams("mgvalleys_np_inter_valley_slope", np_inter_valley_slope);
	settings->setNoiseParams("mgvalleys_np_rivers",             np_rivers);
	settings->setNoiseParams("mgvalleys_np_terrain_height",     np_terrain_height);
	settings->setNoiseParams("mgvalleys_np_valley_depth",       np_valley_depth);
	settings->setNoiseParams("mgvalleys_np_valley_profile",     np_valley_profile);

	settings->setNoiseParams("mgvalleys_np_cave1",              np_cave1);
	settings->setNoiseParams("mgvalleys_np_cave2",              np_cave2);
	settings->setNoiseParams("mgvalleys_np_cavern",             np_cavern);
	settings->setNoiseParams("mgvalleys_np_dungeons",           np_dungeons);
#if IS_VOPI_ENGINE
	settings->setNoiseParams("mgvalleys_np_mountain",           np_mountain);
	settings->setNoiseParams("mgvalleys_np_mountain_height",    np_mountain_height);
	settings->setNoiseParams("mgvalleys_np_wetland_pools",      np_wetland_pools);
#endif
}


void MapgenValleysParams::setDefaultSettings(Settings *settings)
{
#if IS_VOPI_ENGINE
	settings->setDefault("mgvalleys_spflags", flagdesc_mapgen_valleys,
		MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_ALT_DRY | MGVALLEYS_SEA_LEVEL_RIVERS |
		MGVALLEYS_MOUNTAINS | MGVALLEYS_REMOVE_FLOATERS);
#else
	settings->setDefault("mgvalleys_spflags", flagdesc_mapgen_valleys,
		MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_VARY_RIVER_DEPTH | MGVALLEYS_ALT_DRY);
#endif
}


/////////////////////////////////////////////////////////////////


void MapgenValleys::makeChunk(BlockMakeData *data)
{
	// Pre-conditions
	assert(data->vmanip);
	assert(data->nodedef);

	//TimeTaker t("makeChunk");

	this->generating = true;
	this->vm = data->vmanip;
	this->ndef = data->nodedef;
	// The phases are timed for the profiler while it prints, and the counts
	// of the biome generator start from zero for a mapchunk that is
#if IS_VOPI_ENGINE
	profile_phases = g_settings->getFloat("profiler_print_interval") > 0.0f;
	if (profile_phases)
		m_bgen->resetProfile();
#else
	profile_phases = g_settings->getFloat("profiler_print_interval") > 0.0f;
#endif

	v3s16 blockpos_min = data->blockpos_min;
	v3s16 blockpos_max = data->blockpos_max;
	node_min = blockpos_min * MAP_BLOCKSIZE;
	node_max = (blockpos_max + v3s16(1, 1, 1)) * MAP_BLOCKSIZE - v3s16(1, 1, 1);
	full_node_min = (blockpos_min - 1) * MAP_BLOCKSIZE;
	full_node_max = (blockpos_max + 2) * MAP_BLOCKSIZE - v3s16(1, 1, 1);

	blockseed = getBlockSeed2(full_node_min, seed);

	// Generate biome noises. Note this must be executed strictly before
	// generateTerrain, because generateTerrain depends on intermediate
	// biome-related noises.
	MapgenPhaseTimer timer_biome_noise(profile_phases, "Mapgen: 01 biome noise [ms]");
	m_bgen->calcBiomeNoise(node_min);
	timer_biome_noise.finish();

	// Generate terrain
	MapgenPhaseTimer timer_terrain(profile_phases, "Mapgen: 02 terrain [ms]");
	s16 stone_surface_max_y = generateTerrain();
	timer_terrain.finish();

#if IS_VOPI_ENGINE
	// The biome selection of this mapchunk models the bodies of its
	// columns from what the terrain pass computed, the bulk mountain
	// noise and the feet, as far as they reach; taken back when the
	// mapchunk is done, however it ends
	BiomeTerrainChunk lent_chunk;
	lent_chunk.noise_min = v3s16(node_min.X, node_min.Y - 1, node_min.Z);
	lent_chunk.noise_size = v3s16(csize.X, csize.Y + 2, csize.Z);
	lent_chunk.mountain_noise = mountain_noise_ready ? noise_mountain->result : nullptr;
	lent_chunk.chunk_min = v2s16(node_min.X, node_min.Z);
	lent_chunk.chunk_size = v2s16(csize.X, csize.Z);
	lent_chunk.foot = feet_ready ? foot_dil.data() : nullptr;
	struct ChunkLoan {
		BiomeGenOriginal *bgen;
		~ChunkLoan() { bgen->setTerrainChunk(nullptr); }
	} chunk_loan{m_bgen};
	m_bgen->setTerrainChunk(&lent_chunk);
#endif

	// Create heightmap
	MapgenPhaseTimer timer_heightmap(profile_phases, "Mapgen: 03 heightmap [ms]");
	updateHeightmap(node_min, node_max);
	timer_heightmap.finish();

	// Place biome-specific nodes and build biomemap
	if (flags & MG_BIOMES) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 04 biomes [ms]");
		generateBiomes();
#if IS_VOPI_ENGINE
		// The top of every column as the biome pass leaves it, lids on
		// water included: the biomemap is selected again where the caves,
		// the floor or the floating piece removal move it
		biome_heightmap.resize((size_t)csize.X * csize.Z);
		u32 index = 0;
		for (s16 z = node_min.Z; z <= node_max.Z; z++)
		for (s16 x = node_min.X; x <= node_max.X; x++, index++)
			biome_heightmap[index] = findGroundLevel(v2s16(x, z),
				node_min.Y, node_max.Y);
#endif
	}

	// Generate tunnels, caverns and large randomwalk caves
	if (flags & MG_CAVES) {
		// Generate tunnels first as caverns confuse them
		MapgenPhaseTimer timer_tunnels(profile_phases, "Mapgen: 05 caves tunnels [ms]");
		generateCavesNoiseIntersection(stone_surface_max_y);
		timer_tunnels.finish();

		// Generate caverns
		MapgenPhaseTimer timer_caverns(profile_phases, "Mapgen: 06 caves caverns [ms]");
		bool near_cavern = generateCavernsNoise(stone_surface_max_y);
		timer_caverns.finish();

		// Generate large randomwalk caves
		MapgenPhaseTimer timer_random_walk(profile_phases, "Mapgen: 07 caves random walk [ms]");
		if (near_cavern)
			// Disable large randomwalk caves in this mapchunk by setting
			// 'large cave depth' to world base. Avoids excessive liquid in
			// large caverns and floating blobs of overgenerated liquid.
			generateCavesRandomWalk(stone_surface_max_y,
				-MAX_MAP_GENERATION_LIMIT);
		else
			generateCavesRandomWalk(stone_surface_max_y, large_cave_depth);
	}

#if IS_VOPI_ENGINE
	if (floor_y >= vm->m_area.MinEdge.Y) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 08 floor [ms]");
		// Solid floor: every void and liquid at or below 'floor_y' is stone
		// again, so the tunnels, caves and caverns carved above end there.
		// Done over the whole generation area, border included: the random
		// walk caves of a neighbouring mapchunk reach into that border, and
		// when the neighbour is generated later its caves would otherwise
		// reopen a floor already laid here. Solid nodes are left alone.
		MapNode n_stone(c_stone);
		const v3s16 &vmin = vm->m_area.MinEdge;
		const v3s16 &vmax = vm->m_area.MaxEdge;
		s16 y_top = std::min(floor_y, vmax.Y);
		for (s16 z = vmin.Z; z <= vmax.Z; z++)
		for (s16 y = vmin.Y; y <= y_top; y++) {
			u32 vi = vm->m_area.index(vmin.X, y, z);
			for (s16 x = vmin.X; x <= vmax.X; x++, vi++) {
				content_t c = vm->m_data[vi].getContent();
				if (c == CONTENT_IGNORE || !ndef->get(c).walkable)
					vm->m_data[vi] = n_stone;
			}
		}
	}

	if (spflags & MGVALLEYS_REMOVE_FLOATERS) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 09 floaters [ms]");
		// After the caves, which cut pieces loose, and before anything is
		// placed in or on them
		updateHeightmap(node_min, node_max);
		removeFloaters();
	}
	// The caves, the floor and the removal moved surfaces the biomemap was
	// selected at: the heightmap and the biomemap follow the surface left,
	// for the ores, the decorations and the dust placed from here on
	MapgenPhaseTimer timer_reselect(profile_phases, "Mapgen: 10 reselect biomes [ms]");
	updateHeightmap(node_min, node_max);
	if (flags & MG_BIOMES)
		reselectBiomes();
	timer_reselect.finish();
#endif

	// Generate the registered ores
	if (flags & MG_ORES) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 11 ores [ms]");
		m_emerge->oremgr->placeAllOres(this, blockseed, node_min, node_max);
	}

	// Dungeon creation
	if (flags & MG_DUNGEONS) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 12 dungeons [ms]");
		generateDungeons(stone_surface_max_y);
	}

	// Generate the registered decorations
	if (flags & MG_DECORATIONS) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 13 decorations [ms]");
		m_emerge->decomgr->placeAllDecos(this, blockseed, node_min, node_max);
	}

	// Sprinkle some dust on top after everything else was generated
	if (flags & MG_BIOMES) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 14 dust [ms]");
		dustTopNodes();
	}

	MapgenPhaseTimer timer_liquid(profile_phases, "Mapgen: 15 liquid [ms]");
	updateLiquid(&data->transforming_liquid, full_node_min, full_node_max);
	timer_liquid.finish();

	if (flags & MG_LIGHT) {
		MapgenPhaseTimer timer(profile_phases, "Mapgen: 16 light [ms]");
		calcLighting(node_min - v3s16(0, 1, 0), node_max + v3s16(0, 1, 0),
			full_node_min, full_node_max);
	}

#if IS_VOPI_ENGINE
	// The counts of the biome generator and its sampler for this mapchunk
	if (profile_phases)
		m_bgen->profileChunk();
#endif

	this->generating = false;

	//printf("makeChunk: %lums\n", t.stop());
}


#if IS_VOPI_ENGINE
ValleysColumnParams::ValleysColumnParams(const MapgenValleysParams &params) :
	water_level(params.water_level),
	river_size_factor(params.river_size / 100.0f),
	river_depth_bed(params.river_depth + 1.0f),
	river_valley_width(params.river_valley_width),
	river_bank_height(params.river_bank_height),
	mountain_river_width(std::fmax(params.mountain_river_width, 0.01f)),
	sea_level_rivers(params.spflags & MGVALLEYS_SEA_LEVEL_RIVERS),
	wetlands(params.spflags & MGVALLEYS_WETLANDS),
	wetland_base_min(params.wetland_base_min),
	wetland_base_max(std::fmax(params.wetland_base_max, params.wetland_base_min)),
	wetland_valley_depth_max(std::fmax(params.wetland_valley_depth_max, 0.0f)),
	wetland_fade(std::fmin(std::fmax(params.wetland_fade, 0.01f), 1.0f)),
	wetland_height(params.wetland_height),
	wetland_pool_depth(std::fmax((float)params.wetland_pool_depth, 1.0f)),
	wetland_pool_edge(std::fmin(std::fmax(params.wetland_pool_edge, 0.01f), 1.0f))
{
}


// The ramp of a bound: 'wetland_fade' of the bound's own value, half a
// node at the least, so the ramp at the coast, where the bound is a few
// nodes, is a few nodes wide and the beach keeps its strip, while the ramp
// at the inland bound, tens of nodes up, is a hillside
static inline float wetlandRamp(float bound, float fade)
{
	return std::fmax(std::fabs(bound) * fade, 0.5f);
}

float valleysWetlandWeight(const ValleysColumnParams &p, float n_terrain_height,
	float n_valley)
{
	if (!p.wetlands)
		return 0.0f;
	float valley_d = n_valley * n_valley;
	float region_level = n_terrain_height + valley_d;
	// Distance inside each bound, in fractions of its ramp
	float t = std::fmin(
		(p.wetland_base_max - region_level) /
			wetlandRamp(p.wetland_base_max, p.wetland_fade),
		(region_level - p.wetland_base_min) /
			wetlandRamp(p.wetland_base_min, p.wetland_fade));
	t = std::fmin(t, (p.wetland_valley_depth_max - valley_d) /
		wetlandRamp(p.wetland_valley_depth_max, p.wetland_fade));
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}


// The terrain of one column from its 2D noise values: the river bank
// level, the valley rising away from the river, the amplitude of the 3D
// relief, the river surface, the channel where the river runs, and the
// form of the column. The generator feeds it the bulk noise of the area,
// the biome terrain sampler the scalar noise of a column: one model.
ValleysColumn calcValleysColumn(const ValleysColumnParams &p, float n_slope,
	float n_rivers, float n_terrain_height, float n_valley, float n_valley_profile,
	float n_wetland)
{
	float valley_d = n_valley * n_valley;
	// 'base' represents the level of the river banks
	float base = n_terrain_height + valley_d;
	// 'river' represents the distance from the river edge
	float river = std::fabs(n_rivers) - p.river_size_factor;
	// Use the curve of the function 1-exp(-(x/a)^2) to model valleys.
	// 'valley_h' represents the height of the terrain, from the rivers.
	float tv = std::fmax(river / n_valley_profile, 0.0f);
	float valley_pos = 1.0f - std::exp(-tv * tv);
	float valley_h = valley_d * valley_pos;
	// Approximate height of the terrain
	float surface_y = base + valley_h;
	float slope = n_slope * valley_h;
	// River water surface is 1 node below river banks
	float river_y = base - 1.0f;
	bool river_water = false;

	ValleysColumn c;
	// The form keeps the region level before the bank clamp below: that
	// clamp shapes the surface, the region level says how high the land
	// between the valleys would rise
	c.region_level = base;
	c.valley_depth = valley_d;
	c.valley_pos = valley_pos;
	c.wetland = 0.0f;

	// Sea level river channels carry river water below the water line
	if (p.sea_level_rivers) {
		float bank = p.water_level + p.river_bank_height;
		if (base > bank) {
			// River banks never rise above 'river_bank_height' over the
			// sea: the height by which they would is removed, fading out over
			// 'river_valley_width' of the valley profile. The terrain
			// beyond keeps its height, so the valley deepens instead
			// and every river is level with the sea.
			float tg = std::fmax(river /
				(n_valley_profile * p.river_valley_width), 0.0f);
			surface_y -= (base - bank) * std::exp(-tg * tg);
			base = bank;
			// River water only below the water line, whatever the bank height
			river_y = p.water_level;
			river_water = river < 0.0f;
			// The 3D relief never exceeds the height left above the
			// bank, so lowered ground does not dip under the sea
			slope = std::fmin(slope, n_slope * (surface_y - base));
		}
	}

	// Wetlands: flat low country sinks to 'wetland_height' over the water
	// line by the weight of the wetland in the column, and where the pool
	// noise is positive, into a pool: the bank shelves down to the first
	// node of water over 'wetland_pool_edge' of the noise, and the pool
	// deepens to 'wetland_pool_depth' nodes as the noise rises to 1, so
	// the flat holds pools the water fills, with islets between them and
	// shallows at their edges. The bank level follows, so a river
	// through the wetland runs level with the pools. The 3D relief stays,
	// a node or two of it, so the flat is low but not a plane. The weight
	// is 0 at the bounds, so the country around keeps its height and the
	// wetland meets it on a slope. The sinking only lowers: ground
	// already under its level, a lake or a sea inside the wetland, keeps
	// its bed. With sea level rivers the bank clamp above has set the
	// bank already, and the river through the wetland keeps the depth of
	// every other. Ground is where the density is positive, so a surface
	// at Y + 0.5 puts the top node at Y
	if (p.wetlands) {
		float wetland = valleysWetlandWeight(p, n_terrain_height, n_valley);
		c.wetland = wetland;
		if (wetland > 0.0f) {
			float bank_level = p.water_level + p.wetland_height + 0.5f;
			float level = bank_level;
			if (n_wetland > 0.0f) {
				float pool_top = p.water_level - 0.5f;
				level = bank_level + (pool_top - bank_level) *
						std::fmin(n_wetland / p.wetland_pool_edge, 1.0f) -
					(p.wetland_pool_depth - 1.0f) * std::fmin(n_wetland, 1.0f);
			}
			if (surface_y > level)
				surface_y += (level - surface_y) * wetland;
			if (!p.sea_level_rivers && base > bank_level) {
				base += (bank_level - base) * wetland;
				river_y = base - 1.0f;
			}
		}
	}

	// Rivers are placed where 'river' is negative
	if (river < 0.0f) {
		// Use the function -sqrt(1-x^2) which models a circle
		float tr = river / p.river_size_factor + 1.0f;
		float depth = p.river_depth_bed *
			std::sqrt(std::fmax(0.0f, 1.0f - tr * tr));
		// There is no logical equivalent to this using rangelim
		surface_y = std::fmin(
			std::fmax(base - depth, p.water_level - 3.0f), surface_y);
		slope = 0.0f;
		// Sea level channels are not held above 'water_level - 3':
		// their depth follows 'river_depth' below the banks
		if (river_water)
			surface_y = std::fmin(base - depth, surface_y);
	}

	c.surface_y = surface_y;
	c.base = base;
	c.slope = slope;
	c.river = river;
	c.river_y = river_y;
	c.valley_profile = n_valley_profile;
	c.river_water = river_water;
	return c;
}


// The rise of the valley, 0 in the river channel and 1 beyond
// 'mountain_river_width' of the valley profile, times the dry share of
// the column: no body rises from a wetland's sunk flat, and one crossing
// the ramp around it tapers with the weight, as it does at a river
float valleysMountainGate(const ValleysColumn &c, float mountain_river_width)
{
	float tm = std::fmax(c.river /
		(c.valley_profile * mountain_river_width), 0.0f);
	return (1.0f - std::exp(-tm * tm)) * (1.0f - c.wetland);
}


float valleysMountainFoot(const NoiseParams &np_mountain, float x, float z,
	float surface_y, float gate, s32 seed)
{
	float ys = std::floor(surface_y + 0.5f);
	float n = NoiseFractal3D(&np_mountain, x, ys, z, seed);
	return std::fmax(n * gate, 0.0f);
}


void MapgenValleys::terrainColumn(float n_slope, float n_rivers,
	float n_terrain_height, float n_valley, float n_valley_profile,
	float n_wetland, Column &c) const
{
	c = calcValleysColumn(column_params, n_slope, n_rivers, n_terrain_height,
		n_valley, n_valley_profile, n_wetland);
}
#else
// The terrain of one column from its 2D noise values: the river bank
// level, the valley rising away from the river, the amplitude of the 3D
// relief, the river surface, and the channel where the river runs.
void MapgenValleys::terrainColumn(float n_slope, float n_rivers,
	float n_terrain_height, float n_valley, float n_valley_profile,
	Column &c) const
{
	float valley_d = n_valley * n_valley;
	// 'base' represents the level of the river banks
	float base = n_terrain_height + valley_d;
	// 'river' represents the distance from the river edge
	float river = std::fabs(n_rivers) - river_size_factor;
	// Use the curve of the function 1-exp(-(x/a)^2) to model valleys.
	// 'valley_h' represents the height of the terrain, from the rivers.
	float tv = std::fmax(river / n_valley_profile, 0.0f);
	float valley_h = valley_d * (1.0f - std::exp(-tv * tv));
	// Approximate height of the terrain
	float surface_y = base + valley_h;
	float slope = n_slope * valley_h;
	// River water surface is 1 node below river banks
	float river_y = base - 1.0f;

	// Rivers are placed where 'river' is negative
	if (river < 0.0f) {
		// Use the function -sqrt(1-x^2) which models a circle
		float tr = river / river_size_factor + 1.0f;
		float depth = (river_depth_bed *
			std::sqrt(std::fmax(0.0f, 1.0f - tr * tr)));
		// There is no logical equivalent to this using rangelim
		surface_y = std::fmin(
			std::fmax(base - depth, (float)(water_level - 3)),
			surface_y);
		slope = 0.0f;
	}

	c.surface_y = surface_y;
	c.base = base;
	c.slope = slope;
	c.river = river;
	c.river_y = river_y;
	c.valley_profile = n_valley_profile;
	c.river_water = false;
}
#endif


MapgenValleys::Column MapgenValleys::columnAt(s16 x, s16 z) const
{
	Column c;
	terrainColumn(
		NoiseFractal2D(&noise_inter_valley_slope->np, x, z, seed),
		NoiseFractal2D(&noise_rivers->np, x, z, seed),
		NoiseFractal2D(&noise_terrain_height->np, x, z, seed),
		NoiseFractal2D(&noise_valley_depth->np, x, z, seed),
		NoiseFractal2D(&noise_valley_profile->np, x, z, seed),
#if IS_VOPI_ENGINE
		noise_wetland_pools ?
			NoiseFractal2D(&noise_wetland_pools->np, x, z, seed) : 0.0f,
#endif
		c);
	return c;
}


#if IS_VOPI_ENGINE
// How much of the mountain body a column may carry: the rise of the valley
// itself, 0 in the river channel and 1 beyond 'mountain_river_width' of
// the valley profile, so mountains grow where the ground climbs out of the
// valley and never dam a river, and nothing of it on a wetland's flat.
float MapgenValleys::mountainGate(const Column &c) const
{
	return valleysMountainGate(c, mountain_river_width);
}


// The foot of the body in a column: the mountain noise at the ground,
// gated, where it is positive. A cap can hang from a foot.
float MapgenValleys::mountainFoot(s16 x, s16 z, const Column &c,
	float gate) const
{
	return valleysMountainFoot(noise_mountain->np, x, z, c.surface_y, gate, seed);
}


// The strongest foot within reach of a point, tapered, as the cap of a
// mapchunk column sees it. Costs a neighbourhood of columns.
float MapgenValleys::spawnFoot(v2s16 p, const Column &c) const
{
	float foot = 0.0f;
	const float taper = 1.0f / (float)(mountain_cap_reach + 1);
	for (s16 dz = -mountain_cap_reach; dz <= mountain_cap_reach; dz++)
	for (s16 dx = -mountain_cap_reach; dx <= mountain_cap_reach; dx++) {
		s16 x = p.X + dx;
		s16 z = p.Y + dz;
		if (NoiseFractal2D(&noise_mountain_height->np, x, z, seed) <= 0.0f)
			continue;
		Column cn = (dx == 0 && dz == 0) ? c : columnAt(x, z);
		float gate = mountainGate(cn);
		if (gate > 0.0f)
			foot = std::fmax(foot, mountainFoot(x, z, cn, gate) *
				(1.0f - (float)std::abs(dx) * taper) *
				(1.0f - (float)std::abs(dz) * taper));
	}
	return foot;
}
#endif


int MapgenValleys::getSpawnLevelAtPoint(v2s16 p)
{
	// Check if in a river channel
	float n_rivers = NoiseFractal2D(&noise_rivers->np, p.X, p.Y, seed);
	if (std::fabs(n_rivers) <= river_size_factor)
		// Unsuitable spawn point
		return MAX_MAP_GENERATION_LIMIT;

	Column c = columnAt(p.X, p.Y);
	float surface_y = c.surface_y;
	float slope = c.slope;
	float river_y = c.river_y;

#if IS_VOPI_ENGINE
	// Mountain body of this column, as in generateTerrain: gate and height.
	// The cap is looked at only once a level is found, below.
	float mnt_gate = 0.0f;
	float mnt_height = 0.0f;
	float mnt_floor = surface_y - 1.5f * std::fabs(slope);
	if ((spflags & MGVALLEYS_MOUNTAINS) && mountain_noise_max > 0.0f) {
		mnt_height = NoiseFractal2D(&noise_mountain_height->np, p.X, p.Y, seed);
		if (mnt_height > 0.0f)
			mnt_gate = mountainGate(c);
	}
#endif

	// Raising the maximum spawn level above 'water_level + 16' is necessary for custom
	// parameters that set average terrain level much higher than water_level.
	s16 max_spawn_y = std::fmax(
		noise_terrain_height->np.offset +
		noise_valley_depth->np.offset * noise_valley_depth->np.offset,
		water_level + 16);

	// Starting spawn search at max_spawn_y + 128 ensures 128 nodes of open
	// space above spawn position. Avoids spawning in possibly sealed voids.
	for (s16 y = max_spawn_y + 128; y >= water_level; y--) {
		float n_fill = NoiseFractal3D(&noise_inter_valley_fill->np, p.X, y, p.Y, seed);
		float surface_delta = (float)y - surface_y;
		float density = slope * n_fill - surface_delta;
#if IS_VOPI_ENGINE
		if (density <= 0.0f && mnt_gate > 0.0f && (float)y > mnt_floor) {
			float n_mountain = NoiseFractal3D(&noise_mountain->np, p.X, y, p.Y, seed);
			density = n_mountain * mnt_gate - surface_delta / mnt_height;
		}
#endif

		if (density > 0.0f) {  // If solid
			// Sometimes surface level is below river water level in places that are not
			// river channels.
			if (y < water_level || y > max_spawn_y || y < (s16)river_y)
				// Unsuitable spawn point
				return MAX_MAP_GENERATION_LIMIT;

#if IS_VOPI_ENGINE
			// The player stands in y + 2 and y + 3. A cap hanging from a
			// foot nearby can fill them; that is checked only here, for the
			// one level that is acceptable otherwise, as it costs a
			// neighbourhood of columns.
			if (mnt_gate > 0.0f && (float)(y + 3) > surface_y &&
					(float)(y + 1) - surface_y < mountain_cap_height) {
				float foot = spawnFoot(p, c);
				for (s16 yy = y + 1; foot > 0.0f && yy <= y + 3; yy++) {
					float delta = (float)yy - surface_y;
					if (delta <= 0.0f || delta >= mountain_cap_height)
						continue;
					float t = delta / mountain_cap_height;
					float cap = mountain_cap * foot * 4.0f * t * (1.0f - t);
					float n_mountain = NoiseFractal3D(&noise_mountain->np,
						p.X, yy, p.Y, seed);
					if ((n_mountain + cap) * mnt_gate - delta / mnt_height > 0.0f)
						// Inside a cap
						return MAX_MAP_GENERATION_LIMIT;
				}
			}
#endif

			// y + 2 because y is surface and due to biome 'dust' nodes.
			return y + 2;
		}
	}
	// Unsuitable spawn position, no ground found
	return MAX_MAP_GENERATION_LIMIT;
}


#if IS_VOPI_ENGINE
// Removes ground left floating above the terrain by the 3D noise or cut
// loose by the caves. Every column whose topmost run
// of ground nodes ends above the column's floor, the level under which the
// base terrain is solid, seeds a flood fill over connected ground nodes. A
// piece that reaches neither that floor nor the edge of the mapchunk is
// deleted, whatever its size; everything standing on the ground, overhangs
// included, is kept. Runs after the caves and before ores and decorations.
// Ground is what is_ground_content says, as for the caves: what a
// decoration laid is neither a piece nor a foothold, so the crown a
// neighbouring mapchunk's tree put into this one before it was generated
// stays as laid, connected to its trunk or not.
void MapgenValleys::removeFloaters()
{
	// Pieces beyond this are kept unexamined, a bound on the work per seed
	const size_t max_size = 65536;
	// Downwards last, so the depth-first fill heads for the floor first
	static const v3s16 dirs[6] = {
		v3s16(1, 0, 0), v3s16(-1, 0, 0), v3s16(0, 0, 1),
		v3s16(0, 0, -1), v3s16(0, 1, 0), v3s16(0, -1, 0)
	};

	const v3s32 &em = vm->m_area.getExtent();
	MapNode n_air(CONTENT_AIR);
	MapNode n_water(c_water_source);

	size_t volume = (size_t)csize.X * csize.Y * csize.Z;
	if (floater_visited.size() != volume)
		floater_visited.assign(volume, 0);
	else
		std::fill(floater_visited.begin(), floater_visited.end(), 0);

	auto local_index = [&](const v3s16 &p) -> size_t {
		return ((size_t)(p.Z - node_min.Z) * csize.Y + (p.Y - node_min.Y)) *
			csize.X + (p.X - node_min.X);
	};
	auto column_floor = [&](const v3s16 &p) -> float {
		return floater_floor[(size_t)(p.Z - node_min.Z) * csize.X +
			(p.X - node_min.X)];
	};
	auto ground = [&](const MapNode &n) -> bool {
		const ContentFeatures &f = ndef->get(n);
		return f.walkable && f.is_ground_content;
	};

	// Marks: 1 on a node of the piece being filled, 2 on a node of a piece
	// found grounded. A fill that touches a grounded node is grounded too.
	// Without that, the nodes a grounded fill left marked but unexplored
	// would wall off a later fill whose only way to the ground runs through
	// them, and a thin crown or overhang standing on the ground would go.
	const u8 M_PIECE = 1;
	const u8 M_GROUNDED = 2;
	std::vector<v3s16> stack;
	std::vector<v3s16> piece;
	u32 index_2d = 0;

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index_2d++) {
		s16 top = heightmap[index_2d];
		float floor = floater_floor[index_2d];
		if (top < node_min.Y || top > node_max.Y || (float)top <= floor)
			continue;
		if (floater_visited[local_index(v3s16(x, top, z))])
			continue;

		// A top that is not ground, a tree's crown, seeds nothing
		u32 vi = vm->m_area.index(x, top, z);
		if (!ground(vm->m_data[vi]))
			continue;

		// Does the topmost run of ground nodes reach the floor?
		s16 y = top;
		while (y >= node_min.Y && (float)y > floor &&
				ground(vm->m_data[vi])) {
			y--;
			VoxelArea::add_y(em, vi, -1);
		}
		if (y < node_min.Y || (float)y <= floor)
			continue;

		stack.clear();
		piece.clear();
		stack.emplace_back(x, top, z);
		floater_visited[local_index(stack.back())] = M_PIECE;
		bool grounded = false;

		while (!stack.empty() && !grounded) {
			v3s16 p = stack.back();
			stack.pop_back();
			piece.push_back(p);
			if (piece.size() > max_size || (float)p.Y <= column_floor(p) ||
					p.X == node_min.X || p.X == node_max.X ||
					p.Y == node_min.Y || p.Y == node_max.Y ||
					p.Z == node_min.Z || p.Z == node_max.Z) {
				grounded = true;
				break;
			}
			for (const v3s16 &d : dirs) {
				v3s16 q = p + d;
				size_t li = local_index(q);
				u8 mark = floater_visited[li];
				if (mark == M_GROUNDED) {
					grounded = true;
					break;
				}
				if (mark != 0)
					continue;
				if (!ground(vm->m_data[vm->m_area.index(q.X, q.Y, q.Z)]))
					continue;
				floater_visited[li] = M_PIECE;
				stack.push_back(q);
			}
		}

		if (grounded) {
			// Everything reached is connected to the ground: the piece and
			// what is still waiting on the stack
			for (const v3s16 &p : piece)
				floater_visited[local_index(p)] = M_GROUNDED;
			for (const v3s16 &p : stack)
				floater_visited[local_index(p)] = M_GROUNDED;
			continue;
		}
		for (const v3s16 &p : piece)
			vm->m_data[vm->m_area.index(p.X, p.Y, p.Z)] =
				(p.Y <= water_level) ? n_water : n_air;
	}
}


// The biome pass records, per column, the biome selected at the first
// stone surface met on the way down, and it runs before the caves, whose
// entrance floors need that record, so before the solid floor and the
// floating piece removal as well. Where that surface went since, eaten by
// a cave or removed as a piece, the record names the biome of a height the
// column no longer has, while the ground under it was laid with the biome
// selected at its own surface: the decorations and the dust, which go by
// the record, would follow the surface that is gone; and where the floor
// laid ground under a column that had none, the record names no biome at
// all. Every column whose top moved since the
// biome pass gets the biome selected at its surface now, as the pass
// selects it: at the ground under the lids the pass lays on water, or,
// with no ground left in the mapchunk, at the liquid surface, or none.
// The nodes stay as laid.
void MapgenValleys::reselectBiomes()
{
	const v3s32 &em = vm->m_area.getExtent();
	u32 index = 0;
	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index++) {
		const s16 top = heightmap[index];
		if (top == biome_heightmap[index])
			continue;
		// Down from the top through the lids and the liquid to the ground
		s16 y = top;
		if (y >= node_min.Y) {
			u32 vi = vm->m_area.index(x, y, z);
			while (y >= node_min.Y) {
				const content_t c = vm->m_data[vi].getContent();
				if (ndef->get(c).walkable && !CONTAINS(water_lids, c))
					break;
				y--;
				VoxelArea::add_y(em, vi, -1);
			}
		}
		if (y < node_min.Y) {
			// No ground in the mapchunk: the liquid surface, which a lid
			// on top is, as the biome pass falls back to for open water
			y = (top >= node_min.Y) ? top :
				findLiquidSurface(v2s16(x, z), node_min.Y, node_max.Y);
		}
		biomemap[index] = (y < node_min.Y) ? BIOME_NONE :
			biomegen->getBiomeAtIndex(index, v3s16(x, y, z))->index;
	}
}
#endif


int MapgenValleys::generateTerrain()
{
	MapNode n_air(CONTENT_AIR);
	MapNode n_river_water(c_river_water_source);
	MapNode n_stone(c_stone);
	MapNode n_water(c_water_source);

	// The terrain noises and the columns cover 'column_reach' beyond the
	// mapchunk on every side
	const s32 reach = column_reach;
	const s32 area_x = csize.X + 2 * reach;
	const s32 area_z = csize.Z + 2 * reach;
	const s32 area_min_x = node_min.X - reach;
	const s32 area_min_z = node_min.Z - reach;

	MapgenPhaseTimer timer_noise_2d(profile_phases, "Terrain: 1 noise 2D [ms]");
	noise_inter_valley_slope->noiseMap2D(area_min_x, area_min_z);
	noise_rivers->noiseMap2D(area_min_x, area_min_z);
	noise_terrain_height->noiseMap2D(area_min_x, area_min_z);
	noise_valley_depth->noiseMap2D(area_min_x, area_min_z);
	noise_valley_profile->noiseMap2D(area_min_x, area_min_z);
	timer_noise_2d.finish();

	MapgenPhaseTimer timer_fill(profile_phases, "Terrain: 2 fill noise 3D [ms]");
	noise_inter_valley_fill->noiseMap3D(node_min.X, node_min.Y - 1, node_min.Z);
	timer_fill.finish();

#if IS_VOPI_ENGINE
	// Wetlands: the pool noise only once a column of the area holds one
	const float *wetland_pools = nullptr;
	if (noise_wetland_pools) {
		MapgenPhaseTimer timer(profile_phases, "Terrain: 3 wetland noise [ms]");
		for (s32 i = 0; i < area_x * area_z; i++) {
			if (valleysWetlandWeight(column_params, noise_terrain_height->result[i],
					noise_valley_depth->result[i]) > 0.0f) {
				noise_wetland_pools->noiseMap2D(area_min_x, area_min_z);
				wetland_pools = noise_wetland_pools->result;
				break;
			}
		}
	}
#endif

	MapgenPhaseTimer timer_columns(profile_phases, "Terrain: 4 columns [ms]");
	columns.resize((size_t)area_x * area_z);
	for (s32 i = 0; i < area_x * area_z; i++)
		terrainColumn(noise_inter_valley_slope->result[i],
			noise_rivers->result[i], noise_terrain_height->result[i],
			noise_valley_depth->result[i], noise_valley_profile->result[i],
#if IS_VOPI_ENGINE
			wetland_pools ? wetland_pools[i] : 0.0f,
#endif
			columns[i]);
	timer_columns.finish();

#if IS_VOPI_ENGINE
	// Mountains: the 2D height and the feet now, the 3D noise once a column
	// needs it, so mapchunks above every mountain or in regions without
	// them skip it
	const bool gen_mountains = (spflags & MGVALLEYS_MOUNTAINS) &&
		mountain_noise_max > 0.0f;
	mountain_noise_ready = false;
	feet_ready = false;
	if (gen_mountains) {
		MapgenPhaseTimer timer(profile_phases, "Terrain: 5 mountain height 2D [ms]");
		noise_mountain_height->noiseMap2D(area_min_x, area_min_z);
		// The feet matter only where the cap band, 'mountain_cap_height'
		// above the ground, reaches into this mapchunk in some column of
		// it; mapchunks under the ground or high above it skip them
		float s_min = (float)MAX_MAP_GENERATION_LIMIT;
		float s_max = -(float)MAX_MAP_GENERATION_LIMIT;
		for (s32 z = 0; z < csize.Z; z++)
		for (s32 x = 0; x < csize.X; x++) {
			float s = columns[(size_t)(z + reach) * area_x + (x + reach)].surface_y;
			s_min = std::fmin(s_min, s);
			s_max = std::fmax(s_max, s);
		}
		feet_ready = s_max + mountain_cap_height > (float)(node_min.Y - 1) &&
			s_min < (float)(node_max.Y + 1);
	}
	if (feet_ready) {
		MapgenPhaseTimer timer(profile_phases, "Terrain: 6 mountain feet [ms]");
		// The foot of the body in every column of the area, then the
		// strongest foot within reach of every column of the mapchunk,
		// tapering with the distance so a cap is widest over its foot and
		// fades out at the reach. That is what the cap of a body hangs
		// from, and a foot beyond the mapchunk casts its cap into it like
		// any other.
		foot.assign((size_t)area_x * area_z, 0.0f);
		for (s32 zi = 0; zi < area_z; zi++)
		for (s32 xi = 0; xi < area_x; xi++) {
			s32 i = zi * area_x + xi;
			if (noise_mountain_height->result[i] <= 0.0f)
				continue;
			float gate = mountainGate(columns[i]);
			if (gate > 0.0f)
				foot[i] = mountainFoot(area_min_x + xi, area_min_z + zi,
					columns[i], gate);
		}
		const float taper = 1.0f / (float)(reach + 1);
		foot_row.assign((size_t)csize.X * area_z, 0.0f);
		for (s32 zi = 0; zi < area_z; zi++)
		for (s32 x = 0; x < csize.X; x++) {
			float m = 0.0f;
			for (s32 dx = -reach; dx <= reach; dx++)
				m = std::fmax(m, foot[zi * area_x + x + reach + dx] *
					(1.0f - (float)std::abs(dx) * taper));
			foot_row[zi * csize.X + x] = m;
		}
		foot_dil.assign((size_t)csize.X * csize.Z, 0.0f);
		for (s32 z = 0; z < csize.Z; z++)
		for (s32 x = 0; x < csize.X; x++) {
			float m = 0.0f;
			for (s32 dz = -reach; dz <= reach; dz++)
				m = std::fmax(m, foot_row[(z + reach + dz) * csize.X + x] *
					(1.0f - (float)std::abs(dz) * taper));
			foot_dil[z * csize.X + x] = m;
		}
	}
#endif

	const v3s32 &em = vm->m_area.getExtent();
	s16 surface_max_y = -MAX_MAP_GENERATION_LIMIT;
	u32 index_2d = 0;
	// The column loop, and the climate of the columns within it
	MapgenPhaseTimer timer_density(profile_phases, "Terrain: 8 density [ms]");
#if IS_VOPI_ENGINE
	u64 climate_us = 0;
#endif

	for (s16 z = node_min.Z; z <= node_max.Z; z++)
	for (s16 x = node_min.X; x <= node_max.X; x++, index_2d++) {
		const size_t index_area = (size_t)(z - node_min.Z + reach) * area_x +
			(x - node_min.X + reach);
		const Column &col = columns[index_area];
		float surface_y = col.surface_y;
		float base = col.base;
		float slope = col.slope;
		float river_y = col.river_y;

#if IS_VOPI_ENGINE
		bool river_water = col.river_water;
		// Below this the base terrain is solid whatever the 3D relief does
		floater_floor[index_2d] = surface_y - 1.5f * std::fabs(slope);

		// Mountain body of this column: a 3D density anchored at the terrain
		// surface, '(n_mountain + cap) * gate - (y - surface_y) / height',
		// solid where positive. The height, a 2D noise, is the vertical
		// distance the density gradient spans: small mountains where it is
		// small, none at all where it is <= 0. The cap lowers the threshold
		// around a foot, most at half 'mountain_cap_height' above the
		// ground, so a body widens over its foot into overhangs, and two
		// feet within reach of each other join into an arch. Evaluated down
		// to the floater floor as well, so the body fills the relief cut
		// under itself and stands on the ground.
		float mnt_gate = 0.0f;
		float mnt_height = 0.0f;
		float mnt_foot = 0.0f;
		bool column_mountains = false;
		if (gen_mountains) {
			mnt_height = noise_mountain_height->result[index_area];
			if (mnt_height > 0.0f)
				mnt_gate = mountainGate(col);
			if (mnt_gate > 0.0f) {
				mnt_foot = feet_ready ? foot_dil[index_2d] : 0.0f;
				// Highest node the body can reach: the column is skipped
				// above it, and below the floor the relief cannot cut under
				float mnt_ymax = surface_y + std::fmax(
					mountain_noise_max * mnt_gate * mnt_height,
					mnt_foot > 0.0f ? mountain_cap_height : 0.0f);
				column_mountains = mnt_ymax >= (float)(node_min.Y - 1) &&
					(float)(node_max.Y + 1) > floater_floor[index_2d];
			}
		}
		if (column_mountains && !mountain_noise_ready) {
			MapgenPhaseTimer timer(profile_phases, "Terrain: 7 mountain noise 3D [ms]");
			noise_mountain->noiseMap3D(node_min.X, node_min.Y - 1, node_min.Z);
			mountain_noise_ready = true;
		}
#endif

		// Optionally vary river depth according to heat and humidity
		if (spflags & MGVALLEYS_VARY_RIVER_DEPTH) {
			float t_heat = m_bgen->heatmap[index_2d];
			float heat = (spflags & MGVALLEYS_ALT_CHILL) ?
				// Match heat value calculated below in
				// 'Optionally decrease heat with altitude'.
				// In rivers, 'ground height ignoring riverbeds' is 'base'.
				// As this only affects river water we can assume y > water_level.
				t_heat + 5.0f - (base - water_level) * 20.0f / altitude_chill :
				t_heat;
			float delta = m_bgen->humidmap[index_2d] - 50.0f;
			if (delta < 0.0f) {
				float t_evap = (heat - 32.0f) / 300.0f;
				river_y += delta * std::fmax(t_evap, 0.08f);
			}
		}

		// Highest solid node in column
#if !IS_VOPI_ENGINE
		s16 column_max_y = surface_y;
#endif
		u32 index_3d = (z - node_min.Z) * zstride_1u1d + (x - node_min.X);
		u32 index_data = vm->m_area.index(x, node_min.Y - 1, z);

		for (s16 y = node_min.Y - 1; y <= node_max.Y + 1; y++) {
			if (vm->m_data[index_data].getContent() == CONTENT_IGNORE) {
				float n_fill = noise_inter_valley_fill->result[index_3d];
				float surface_delta = (float)y - surface_y;
				// Density = density noise + density gradient
				float density = slope * n_fill - surface_delta;
#if IS_VOPI_ENGINE
				if (density <= 0.0f && column_mountains &&
						(float)y > floater_floor[index_2d]) {
					float cap = 0.0f;
					if (mnt_foot > 0.0f && surface_delta > 0.0f &&
							surface_delta < mountain_cap_height) {
						float t = surface_delta / mountain_cap_height;
						cap = mountain_cap * mnt_foot * 4.0f * t * (1.0f - t);
					}
					density = (noise_mountain->result[index_3d] + cap) * mnt_gate -
						surface_delta / mnt_height;
				}
#endif

				if (density > 0.0f) {
					vm->m_data[index_data] = n_stone; // Stone
					if (y > surface_max_y)
						surface_max_y = y;
#if !IS_VOPI_ENGINE
					if (y > column_max_y)
						column_max_y = y;
#endif
				}
				else if (y <= water_level) {
					vm->m_data[index_data] = n_water; // Water
#if IS_VOPI_ENGINE
					if (river_water)
						vm->m_data[index_data] = n_river_water; // Sea level river
#endif
				} else if (y <= (s16)river_y) {
					vm->m_data[index_data] = n_river_water; // River water
				} else {
					vm->m_data[index_data] = n_air; // Air
				}
			}

			VoxelArea::add_y(em, index_data, 1);
			index_3d += ystride;
		}

#if IS_VOPI_ENGINE
		const u64 climate_start = profile_phases ? porting::getTimeUs() : 0;
		// The maps hold the scalar raw climate of the column since the
		// biome noise was calculated. The corrections, where their flags
		// are on, need the column's bank and surface and go through the
		// biome generator; without them the raw climate stands, checked
		// as a query checks it before the selector takes it.
		if (m_bgen->hasClimateCorrections()) {
			ValleysClimate climate;
			if (!m_bgen->getEffectiveClimate(v2s16(x, z), climate))
				throw InvalidNoiseParamsException("Cannot sample effective biome climate at (" +
					std::to_string(x) + ", " + std::to_string(z) + ")");
			m_bgen->heatmap[index_2d] = climate.heat;
			m_bgen->humidmap[index_2d] = climate.humidity;
		} else if (!BiomeGenOriginal::isSelectableClimate(
				m_bgen->heatmap[index_2d], m_bgen->humidmap[index_2d])) {
			throw InvalidNoiseParamsException("Cannot sample effective biome climate at (" +
				std::to_string(x) + ", " + std::to_string(z) + ")");
		}
		if (profile_phases)
			climate_us += porting::getTimeUs() - climate_start;
#else
		// Optionally increase humidity around rivers
		if (spflags & MGVALLEYS_HUMID_RIVERS) {
			// Compensate to avoid increasing average humidity
			m_bgen->humidmap[index_2d] *= 0.8f;
			// Ground height ignoring riverbeds
			float t_alt = std::fmax(base, (float)column_max_y);
			float water_depth = (t_alt - base) / 4.0f;
			m_bgen->humidmap[index_2d] *=
				1.0f + std::pow(0.5f, std::fmax(water_depth, 1.0f));
		}

		// Optionally decrease humidity with altitude
		if (spflags & MGVALLEYS_ALT_DRY) {
			// Ground height ignoring riverbeds
			float t_alt = std::fmax(base, (float)column_max_y);
			// Only decrease above water_level
			if (t_alt > water_level)
				m_bgen->humidmap[index_2d] -=
					(t_alt - water_level) * 10.0f / altitude_chill;
		}

		// Optionally decrease heat with altitude
		if (spflags & MGVALLEYS_ALT_CHILL) {
			// Compensate to avoid reducing the average heat
			m_bgen->heatmap[index_2d] += 5.0f;
			// Ground height ignoring riverbeds
			float t_alt = std::fmax(base, (float)column_max_y);
			// Only decrease above water_level
			if (t_alt > water_level)
				m_bgen->heatmap[index_2d] -=
					(t_alt - water_level) * 20.0f / altitude_chill;
		}
#endif
	}
	timer_density.finish();
#if IS_VOPI_ENGINE
	if (profile_phases)
		g_profiler->avg("Terrain: 9 climate (in density) [ms]", (float)climate_us / 1000.0f);
#endif

	return surface_max_y;
}
