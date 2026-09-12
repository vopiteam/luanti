// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#if IS_VOPI_ENGINE

#include "irr_v2d.h"
#include <memory>
#include <unordered_map>

struct MapgenValleysParams;

// Metrics of the modeled natural surface, including cliff carving and natural
// floating-component removal, before caves and biome-material changes.
// Height and relief are in nodes;
// slope is in degrees. They describe an eight-node surface sampling lattice,
// not individual voxel faces or modifications made after generation.
struct BiomeTerrain {
	float height = 0.0f;
	float slope = 0.0f;
	float relief = 0.0f;
};

// Terrain form of a column, from the 2D column model alone. It describes the
// place a column occupies in the landscape rather than its materials: the
// region level before the river-bank clamp, the valley depth amplitude, the
// position in the valley profile (0 at the river edge, 1 on the ridge) and
// the mountain mask (0 where no mountain body can rise).
struct BiomeTerrainForm {
	float base = 0.0f;
	float valley_depth = 0.0f;
	float valley_pos = 0.0f;
	float mountain = 0.0f;
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
	virtual BiomeTerrain sample(v2s16 pos) const = 0;
	// Returns false when this sampler has no column model or its context
	// cannot be calculated. This does not calculate slope or relief.
	virtual bool sampleClimate(v2s16, BiomeClimateContext &) const { return false; }
	// Called once before a mapchunk is generated. A sampler may make room so
	// that every column of the chunk stays cached from terrain to biomes.
	virtual void beginChunk() {}
	virtual void resetCache() = 0;
};

// Shared lattice and metric calculation, also usable with synthetic height
// functions. The lattice is aligned to world coordinates, including negative
// coordinates, and is independent of the chunk currently being generated.
class HeightmapBiomeTerrainSampler : public BiomeTerrainSampler {
public:
	BiomeTerrain sample(v2s16 pos) const final;
	void resetCache() override;

protected:
	virtual float sampleHeight(s32 x, s32 z) const = 0;

private:
	float heightAt(s32 x, s32 z) const;
	BiomeTerrain latticeAt(s32 x, s32 z) const;
	mutable std::unordered_map<u64, float> m_heights;
	mutable std::unordered_map<u64, BiomeTerrain> m_metrics;
};

std::unique_ptr<BiomeTerrainSampler> createValleysBiomeTerrainSampler(
		const MapgenValleysParams &params);

#endif
