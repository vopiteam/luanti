// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#if IS_VOPI_ENGINE

#include "irr_v2d.h"
#include "irr_v3d.h"
#include <memory>

struct MapgenValleysParams;

// Terrain form of a column, from the 2D column model alone. It describes the
// place a column occupies in the landscape rather than its materials: the
// region level before the river-bank clamp, the valley depth amplitude, the
// position in the valley profile (0 at the river edge, 1 on the ridge), the
// mountain mask (0 where no mountain body can rise) and the body: the
// height the mountain body, with the cap it hangs from the feet around
// the column, reaches over the terrain at the column, from the density
// sampled up the column, within a few nodes of the modeled surface, 0
// where no body stands on the column. The mask says how tall a body can
// be in the region, the body whether and how far one rises here. The
// wetland is the weight of the wetland in the column (mapgen_valleys.h),
// 0 outside it and 1 where the ground is sunk to the water line.
struct BiomeTerrainForm {
	float base = 0.0f;
	float valley_depth = 0.0f;
	float valley_pos = 0.0f;
	float mountain = 0.0f;
	float body = 0.0f;
	float wetland = 0.0f;
};

// Exact-column context for climate corrections and form selection,
// independent of the current generation pass. The height is the truncated,
// world-bounded 2D surface; the bank is the modeled river-bank level.
struct BiomeClimateContext {
	float river_bank_height = 0.0f;
	s16 column_max_y = 0;
	BiomeTerrainForm form;
	// Whether a mountain body can rise on the column at all. Where it can,
	// the body of the form is modeled when it is asked for, and NaN until
	// then; where it cannot, the body is 0.
	bool body_capable = false;
};

// What a generator has of the mapchunk it is generating, lent to the
// sampler for the bodies of the columns inside it: the bulk 3D mountain
// noise over the mapchunk with its node of overgeneration, and the
// strongest tapered foot within reach of every column, the one the cap
// hangs from. The sampler would read the same points from the scalar
// noise, which the bulk noise matches to its rounding; a sample above the
// buffer, a column outside the mapchunk or a mapchunk without the noise
// still reads the scalar noise. Lent after the terrain pass and taken back
// when the mapchunk is done, so a query reads the scalar noise alone, and
// a body modeled from a loan is kept for that mapchunk only, so what a
// mapchunk selects does not depend on the mapchunks generated before it.
struct BiomeTerrainChunk {
	v3s16 noise_min;
	v3s16 noise_size;
	const float *mountain_noise = nullptr;
	v2s16 chunk_min;
	v2s16 chunk_size;
	const float *foot = nullptr;
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
	// The same context without the body of the form, which is left as the
	// sampler holds it and is not to be read: what the climate corrections
	// need of a column, the bank and the surface, which never costs the body.
	virtual bool sampleClimateHeights(v2s16 pos, BiomeClimateContext &out) const
	{
		return sampleClimate(pos, out);
	}
	// Called once before a mapchunk is generated. A sampler may make room so
	// that every column of the chunk stays cached from terrain to biomes.
	virtual void beginChunk() {}
	// The counts of a mapchunk for the profiler: zeroed before a mapchunk
	// that is profiled, reported and zeroed after it. Between profiled
	// mapchunks they keep counting and mean nothing.
	virtual void resetProfile() const {}
	virtual void profileChunk() const {}
	// The mapchunk a generator is generating, lent for the bodies of its
	// columns, and taken back with nullptr.
	virtual void setChunk(const BiomeTerrainChunk *) {}
};

std::unique_ptr<BiomeTerrainSampler> createValleysBiomeTerrainSampler(
		const MapgenValleysParams &params);

#endif
