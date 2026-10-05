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


#pragma once

#include "mapgen.h"
#include <vector>

#define MGVALLEYS_ALT_CHILL        0x01
#define MGVALLEYS_HUMID_RIVERS     0x02
#define MGVALLEYS_VARY_RIVER_DEPTH 0x04
#define MGVALLEYS_ALT_DRY          0x08
#if IS_VOPI_ENGINE
#define MGVALLEYS_SEA_LEVEL_RIVERS 0x10
#define MGVALLEYS_REMOVE_FLOATERS  0x40
#define MGVALLEYS_MOUNTAINS        0x80
#define MGVALLEYS_WETLANDS         0x100
#endif

class BiomeGenOriginal;

extern const FlagDesc flagdesc_mapgen_valleys[];

#if IS_VOPI_ENGINE
struct MapgenValleysParams;

struct ValleysClimate {
	float heat;
	float humidity;
};

// Apply the climate corrections to one column's raw climate. The caller supplies
// the river-bank level and column_max_y of the deterministic modeled column.
// Neither is an arbitrary query Y. Results are not clamped to 0..100.
// altitude_chill is an integer distance in engine settings; values below 1
// are treated as 1, including zero from older configurations.
ValleysClimate calcValleysClimate(float heat, float humidity,
	float base, s16 column_max_y, int water_level, float altitude_chill, u32 flags);

// The 2D column model of Valleys, shared by the generator and the biome
// terrain sampler so that a point query and generation describe a column
// the same way. These are the few parameters the model depends on.
struct ValleysColumnParams {
	float water_level;
	float river_size_factor;
	float river_depth_bed;
	float river_valley_width;
	float river_bank_height;
	float mountain_river_width;
	bool sea_level_rivers;
	// Wetlands: the region level and the valley depth between which flat
	// low country sinks to the water line, the fraction of each bound
	// over which the sinking fades in from it, the ground left over the
	// water, the depth of the pools under it and the band of the pool
	// noise over which a bank shelves down into a pool
	bool wetlands;
	float wetland_base_min;
	float wetland_base_max;
	float wetland_valley_depth_max;
	float wetland_fade;
	float wetland_height;
	float wetland_pool_depth;
	float wetland_pool_edge;

	explicit ValleysColumnParams(const MapgenValleysParams &params);
};

// The terrain of one column, from its 2D noise values
struct ValleysColumn {
	float surface_y;      // terrain surface
	float base;           // river bank level
	float slope;          // amplitude of the 3D relief
	float river;          // distance from the river edge, negative inside
	float river_y;        // river water surface
	float valley_profile;
	bool river_water;     // sea level channel carrying river water
	// The form of the column, for biome selection: the region level before
	// the river-bank clamp, the valley depth amplitude, and the position in
	// the valley profile, 0 at the river edge and 1 on the ridge
	float region_level;
	float valley_depth;
	float valley_pos;
	// The weight of the wetland, 0 outside it and 1 deep inside
	float wetland;
};

// The weight of the wetland in a column: 1 deep inside its bounds, 0 at
// them and beyond, a smooth ramp over 'wetland_fade' of each range
// between, from the two noises that make the region level and the valley
// depth. 0 whenever wetlands are off
float valleysWetlandWeight(const ValleysColumnParams &params,
	float n_terrain_height, float n_valley);

ValleysColumn calcValleysColumn(const ValleysColumnParams &params, float n_slope,
	float n_rivers, float n_terrain_height, float n_valley, float n_valley_profile,
	float n_wetland);

// The gate of the mountain body in a column: 0 in the river channel and 1
// beyond 'mountain_river_width' of the valley profile
float valleysMountainGate(const ValleysColumn &c, float mountain_river_width);

// The foot of the mountain body in a column: the 3D noise at the terrain
// surface, through the gate, where positive
float valleysMountainFoot(const NoiseParams &np_mountain, float x, float z,
	float surface_y, float gate, s32 seed);
#endif


struct MapgenValleysParams : public MapgenParams {
	u16 altitude_chill = 90;
	u16 river_depth = 4;
	u16 river_size = 5;
#if IS_VOPI_ENGINE
	float river_valley_width = 1.0f;
	u16 river_bank_height = 2;
	s16 floor_y = -31000;
	float mountain_river_width = 0.4f;
	float mountain_cap = 1.6f;
	u16 mountain_cap_height = 44;
	u16 mountain_cap_reach = 14;
	float wetland_base_min = -1.0f;
	float wetland_base_max = 25.0f;
	float wetland_valley_depth_max = 2.0f;
	float wetland_fade = 0.25f;
	u16 wetland_height = 1;
	u16 wetland_pool_depth = 2;
	float wetland_pool_edge = 0.5f;
#endif

	float cave_width = 0.09f;
	s16 large_cave_depth = -33;
	u16 small_cave_num_min = 0;
	u16 small_cave_num_max = 0;
	u16 large_cave_num_min = 0;
	u16 large_cave_num_max = 2;
	float large_cave_flooded = 0.5f;
	s16 cavern_limit = -256;
	s16 cavern_taper = 192;
	float cavern_threshold = 0.6f;
	s16 dungeon_ymin = -31000;
	s16 dungeon_ymax = 63;

	NoiseParams np_filler_depth;
	NoiseParams np_inter_valley_fill;
	NoiseParams np_inter_valley_slope;
	NoiseParams np_rivers;
	NoiseParams np_terrain_height;
	NoiseParams np_valley_depth;
	NoiseParams np_valley_profile;

	NoiseParams np_cave1;
	NoiseParams np_cave2;
	NoiseParams np_cavern;
	NoiseParams np_dungeons;
#if IS_VOPI_ENGINE
	NoiseParams np_mountain;
	NoiseParams np_mountain_height;
	NoiseParams np_wetland_pools;
#endif

	MapgenValleysParams();
	~MapgenValleysParams() = default;

	void readParams(const Settings *settings);
	void writeParams(Settings *settings) const;
	void setDefaultSettings(Settings *settings);
};


class MapgenValleys : public MapgenBasic {
public:

	MapgenValleys(MapgenValleysParams *params,
		EmergeParams *emerge);
	~MapgenValleys();

	virtual MapgenType getType() const { return MAPGEN_VALLEYS; }

	virtual void makeChunk(BlockMakeData *data);
	int getSpawnLevelAtPoint(v2s16 p);

private:
	BiomeGenOriginal *m_bgen = nullptr;

	float altitude_chill;
	float river_depth_bed;
	float river_size_factor;
#if IS_VOPI_ENGINE
	float river_valley_width;
	float river_bank_height;
	s16 floor_y;
	float mountain_river_width;
	float mountain_cap;
	float mountain_cap_height;
	s16 mountain_cap_reach;
	// Largest value 'np_mountain' can take, from its parameters: bounds the
	// height a mountain body can reach above the terrain surface
	float mountain_noise_max;
#endif

	// The terrain of one column, from the 2D noises
#if IS_VOPI_ENGINE
	using Column = ValleysColumn;
	ValleysColumnParams column_params;
#else
	struct Column {
		float surface_y;      // terrain surface
		float base;           // river bank level
		float slope;          // amplitude of the 3D relief
		float river;          // distance from the river edge, negative inside
		float river_y;        // river water surface
		float valley_profile;
		bool river_water;     // sea level channel carrying river water
	};
#endif
#if IS_VOPI_ENGINE
	void terrainColumn(float n_slope, float n_rivers, float n_terrain_height,
		float n_valley, float n_valley_profile, float n_wetland, Column &c) const;
#else
	void terrainColumn(float n_slope, float n_rivers, float n_terrain_height,
		float n_valley, float n_valley_profile, Column &c) const;
#endif
	Column columnAt(s16 x, s16 z) const;
	// Every column of the generation area: the mapchunk and 'column_reach'
	// around it, which is how far the 2D terrain noises are computed
	std::vector<Column> columns;
	s16 column_reach = 0;

	Noise *noise_inter_valley_fill = nullptr;
	Noise *noise_inter_valley_slope = nullptr;
	Noise *noise_rivers = nullptr;
	Noise *noise_terrain_height = nullptr;
	Noise *noise_valley_depth = nullptr;
	Noise *noise_valley_profile = nullptr;

#if IS_VOPI_ENGINE
	// Mountain body: a 3D density anchored at the terrain surface, and the
	// 2D height it fades over, which switches mountains off where it is <= 0
	Noise *noise_mountain = nullptr;
	Noise *noise_mountain_height = nullptr;
	// Wetlands: the 2D pool noise, computed for a generation area only
	// once a column of it holds a wetland
	Noise *noise_wetland_pools = nullptr;
	// Where the body touches the ground, per column of the area, and the
	// strongest foot within 'mountain_cap_reach' of every mapchunk column
	std::vector<float> foot;
	std::vector<float> foot_row;
	std::vector<float> foot_dil;
	// Whether the terrain pass computed the 3D mountain noise and the feet
	// for the mapchunk at hand, which the biome selection then reads
	bool mountain_noise_ready = false;
	bool feet_ready = false;
	float mountainGate(const Column &c) const;
	float mountainFoot(s16 x, s16 z, const Column &c, float gate) const;
	float spawnFoot(v2s16 p, const Column &c) const;
	// One mark per node of the mapchunk, reused between mapchunks
	std::vector<u8> floater_visited;
	// Per column: level under which the base terrain is solid whatever
	// the 3D noise does, so any piece reaching it stands on the ground
	std::vector<float> floater_floor;
	void removeFloaters();
	// The top of every column as the biome pass left it, to find the
	// columns whose surface the caves, the floor or the removal moved
	// since; and the walkable nodes the pass lays on water, ice for one,
	// which top a column without being its ground
	std::vector<s16> biome_heightmap;
	std::vector<content_t> water_lids;
	void reselectBiomes();
#endif

	virtual int generateTerrain();
};
