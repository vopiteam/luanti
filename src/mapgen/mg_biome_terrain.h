// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#if IS_VOPI_ENGINE

#include "irr_v2d.h"
#include <memory>

struct MapgenValleysParams;

// Terrain form of a column, from the 2D column model alone. It describes the
// place a column occupies in the landscape rather than its materials: the
// region level before the river-bank clamp, the valley depth amplitude, the
// position in the valley profile (0 at the river edge, 1 on the ridge), the
// mountain mask (0 where no mountain body can rise) and the body: the
// height the mountain body reaches over the terrain at the column, from
// the body's density sampled up the column, within a few nodes of the
// modeled surface, 0 where no body stands on the column. The mask says
// how tall a body can be in the region, the body whether and how far one
// rises here.
struct BiomeTerrainForm {
	float base = 0.0f;
	float valley_depth = 0.0f;
	float valley_pos = 0.0f;
	float mountain = 0.0f;
	float body = 0.0f;
};

// Exact-column context for climate corrections and form selection,
// independent of the current generation pass. The height is the truncated,
// world-bounded 2D surface; the bank is the modeled river-bank level.
struct BiomeClimateContext {
	float river_bank_height = 0.0f;
	s16 column_max_y = 0;
	BiomeTerrainForm form;
};

// Like BiomeGen, a sampler with caches belongs to one thread. A clone has the
// same immutable terrain parameters and starts with empty caches. Every cached
// value is a function of the parameters and the world position alone, so the
// caches are kept across mapchunks: bounded, never invalidated by a chunk.
class BiomeTerrainSampler {
public:
	virtual ~BiomeTerrainSampler() = default;
	virtual std::unique_ptr<BiomeTerrainSampler> clone() const = 0;
	// The highest node of the modeled natural surface of a column: the base
	// 3D density with mountain bodies, the solid floor and natural
	// floating-component removal, before caves and biome material
	// changes. Not the climate height, which sampleClimate gives.
	virtual float sampleHeight(v2s16 pos) const = 0;
	// Returns false when this sampler has no column model or its context
	// cannot be calculated. This does not model the 3D surface.
	virtual bool sampleClimate(v2s16, BiomeClimateContext &) const { return false; }
	// Called once before a mapchunk is generated. A sampler may make room so
	// that every column of the chunk stays cached from terrain to biomes.
	virtual void beginChunk() {}
};

std::unique_ptr<BiomeTerrainSampler> createValleysBiomeTerrainSampler(
		const MapgenValleysParams &params);

#endif
