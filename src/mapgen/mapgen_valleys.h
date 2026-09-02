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
#if IS_VOPI_ENGINE
#include <vector>
#endif

#define MGVALLEYS_ALT_CHILL        0x01
#define MGVALLEYS_HUMID_RIVERS     0x02
#define MGVALLEYS_VARY_RIVER_DEPTH 0x04
#define MGVALLEYS_ALT_DRY          0x08
#if IS_VOPI_ENGINE
#define MGVALLEYS_SEA_LEVEL_RIVERS 0x10
#define MGVALLEYS_CARVE_CLIFFS     0x20
#define MGVALLEYS_REMOVE_FLOATERS  0x40
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
#endif

	Noise *noise_inter_valley_fill = nullptr;
	Noise *noise_inter_valley_slope = nullptr;
	Noise *noise_rivers = nullptr;
	Noise *noise_terrain_height = nullptr;
	Noise *noise_valley_depth = nullptr;
	Noise *noise_valley_profile = nullptr;

#if IS_VOPI_ENGINE
	Noise *noise_carve = nullptr;
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
