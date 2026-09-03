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
#define MGVALLEYS_CARVE_CLIFFS     0x20
#define MGVALLEYS_REMOVE_FLOATERS  0x40
#define MGVALLEYS_MOUNTAINS        0x80
#endif

class BiomeGenOriginal;

extern const FlagDesc flagdesc_mapgen_valleys[];


struct MapgenValleysParams : public MapgenParams {
	u16 altitude_chill = 90;
	u16 river_depth = 4;
	u16 river_size = 5;
#if IS_VOPI_ENGINE
	float river_valley_width = 1.0f;
	u16 river_bank_height = 2;
	s16 floor_y = -31000;
	u16 carve_zero_height = 16;
	u16 carve_reach = 8;
	float carve_undercut = 0.3f;
	float mountain_river_width = 0.4f;
	float mountain_cap = 1.6f;
	u16 mountain_cap_height = 44;
	u16 mountain_cap_reach = 14;
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
	NoiseParams np_carve;
	NoiseParams np_mountain;
	NoiseParams np_mountain_height;
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
	float carve_zero_height;
	s16 carve_reach;
	float carve_undercut;
	float mountain_river_width;
	float mountain_cap;
	float mountain_cap_height;
	s16 mountain_cap_reach;
	// Largest value 'np_mountain' can take, from its parameters: bounds the
	// height a mountain body can reach above the terrain surface
	float mountain_noise_max;
#endif

	// The terrain of one column, from the 2D noises
	struct Column {
		float surface_y;      // terrain surface
		float base;           // river bank level
		float slope;          // amplitude of the 3D relief
		float river;          // distance from the river edge, negative inside
		float river_y;        // river water surface
		float valley_profile;
		bool river_water;     // sea level channel carrying river water
	};
	void terrainColumn(float n_slope, float n_rivers, float n_terrain_height,
		float n_valley, float n_valley_profile, Column &c) const;
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
	Noise *noise_carve = nullptr;
	// Mountain body: a 3D density anchored at the terrain surface, and the
	// 2D height it fades over, which switches mountains off where it is <= 0
	Noise *noise_mountain = nullptr;
	Noise *noise_mountain_height = nullptr;
	// Where the body touches the ground, per column of the area, and the
	// strongest foot within 'mountain_cap_reach' of every mapchunk column
	std::vector<float> foot;
	std::vector<float> foot_row;
	std::vector<float> foot_dil;
	float mountainGate(const Column &c) const;
	float mountainFoot(s16 x, s16 z, const Column &c, float gate) const;
	float spawnFoot(v2s16 p, const Column &c) const;
	// Per column: final terrain surface and river bank level, for the
	// cliff carving and the floating piece removal
	std::vector<float> surface_cache;
	std::vector<float> bank_cache;
	void carveCliffs();

	// One mark per node of the mapchunk, reused between mapchunks
	std::vector<u8> floater_visited;
	// Per column: level under which the base terrain is solid whatever
	// the 3D noise does, so any piece reaching it stands on the ground
	std::vector<float> floater_floor;
	void removeFloaters();
#endif

	virtual int generateTerrain();
};
