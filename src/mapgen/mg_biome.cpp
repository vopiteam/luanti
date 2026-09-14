// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2014-2018 kwolekr, Ryan Kwolek <kwolekr@minetest.net>
// Copyright (C) 2014-2018 paramat

#include "mg_biome.h"
#include "mg_decoration.h"
#include "emerge.h"
#include "server.h"
#include "nodedef.h"
#include "settings.h"

#include <algorithm>
#if IS_VOPI_ENGINE
#include "mapgen_valleys.h"
#include <cmath>
#endif

///////////////////////////////////////////////////////////////////////////////


BiomeManager::BiomeManager(Server *server) :
	ObjDefManager(server, OBJDEF_BIOME)
{
	m_server = server;

	// Create default biome to be used in case none exist
	Biome *b = new Biome;
	b->name            = "default";

	b->m_nodenames.emplace_back("mapgen_stone");
	b->m_nodenames.emplace_back("mapgen_stone");
	b->m_nodenames.emplace_back("mapgen_stone");
	b->m_nodenames.emplace_back("mapgen_water_source");
	b->m_nodenames.emplace_back("mapgen_water_source");
	b->m_nodenames.emplace_back("mapgen_river_water_source");
	b->m_nodenames.emplace_back("mapgen_stone");
	b->m_nodenames.emplace_back("ignore");
	b->m_nodenames.emplace_back("ignore");
	b->m_nnlistsizes.push_back(1);
	b->m_nodenames.emplace_back("ignore");
	b->m_nodenames.emplace_back("ignore");
	b->m_nodenames.emplace_back("ignore");
#if IS_VOPI_ENGINE
	b->m_nodenames.emplace_back("ignore");
#endif
	m_ndef->pendNodeResolve(b);

	add(b);
}


void BiomeManager::clear()
{
	EmergeManager *emerge = m_server->getEmergeManager();

	if (emerge) {
		// Remove all dangling references in Decorations
		DecorationManager *decomgr = emerge->getWritableDecorationManager();
		for (size_t i = 0; i != decomgr->getNumObjects(); i++) {
			Decoration *deco = (Decoration *)decomgr->getRaw(i);
			deco->biomes.clear();
		}
	}

	// Don't delete the first biome
	for (size_t i = 1; i < m_objects.size(); i++)
		delete (Biome *)m_objects[i];

	m_objects.resize(1);
}


BiomeManager *BiomeManager::clone() const
{
	auto mgr = new BiomeManager();
	assert(mgr);
	ObjDefManager::cloneTo(mgr);
	mgr->m_server = m_server;
	return mgr;
}

////////////////////////////////////////////////////////////////////////////////

void BiomeParamsOriginal::readParams(const Settings *settings)
{
	settings->getNoiseParams("mg_biome_np_heat",           np_heat);
	settings->getNoiseParams("mg_biome_np_heat_blend",     np_heat_blend);
	settings->getNoiseParams("mg_biome_np_humidity",       np_humidity);
	settings->getNoiseParams("mg_biome_np_humidity_blend", np_humidity_blend);
}


void BiomeParamsOriginal::writeParams(Settings *settings) const
{
	settings->setNoiseParams("mg_biome_np_heat",           np_heat);
	settings->setNoiseParams("mg_biome_np_heat_blend",     np_heat_blend);
	settings->setNoiseParams("mg_biome_np_humidity",       np_humidity);
	settings->setNoiseParams("mg_biome_np_humidity_blend", np_humidity_blend);
}


////////////////////////////////////////////////////////////////////////////////

BiomeGenOriginal::BiomeGenOriginal(BiomeManager *biomemgr,
	const BiomeParamsOriginal *params, v3s16 chunksize)
{
	m_bmgr   = biomemgr;
	m_params = params;
	m_csize  = chunksize;

	noise_heat           = new Noise(&params->np_heat,
									params->seed, m_csize.X, m_csize.Z);
	noise_humidity       = new Noise(&params->np_humidity,
									params->seed, m_csize.X, m_csize.Z);
	noise_heat_blend     = new Noise(&params->np_heat_blend,
									params->seed, m_csize.X, m_csize.Z);
	noise_humidity_blend = new Noise(&params->np_humidity_blend,
									params->seed, m_csize.X, m_csize.Z);

	heatmap  = noise_heat->result;
	humidmap = noise_humidity->result;

	biomemap = new biome_t[m_csize.X * m_csize.Z];
	// Initialise with the ID of 'BIOME_NONE' so that cavegen can get the
	// fallback biome when biome generation (which calculates the biomemap IDs)
	// is disabled.
	memset(biomemap, 0, sizeof(biome_t) * m_csize.X * m_csize.Z);

	// Calculate cache of Y transition points
	std::vector<s16> values;
	values.reserve(m_bmgr->getNumObjects() * 2);
	for (size_t i = 0; i < m_bmgr->getNumObjects(); i++) {
		Biome *b = (Biome *)m_bmgr->getRaw(i);
		values.push_back(b->max_pos.Y);
		// We scan for biomes from high Y to low Y (top to bottom). Hence,
		// biomes effectively transition at (min_pos.Y - 1).
		if (b->min_pos.Y > -MAX_MAP_GENERATION_LIMIT)
			values.push_back(b->min_pos.Y - 1);
	}

	std::sort(values.begin(), values.end(), std::greater<>());
	values.erase(std::unique(values.begin(), values.end()), values.end());

	m_transitions_y = std::move(values);
}

BiomeGenOriginal::~BiomeGenOriginal()
{
	delete []biomemap;

	delete noise_heat;
	delete noise_humidity;
	delete noise_heat_blend;
	delete noise_humidity_blend;
}

s16 BiomeGenOriginal::getNextTransitionY(s16 y) const
{
	// Find first value that is less than y using binary search
	auto it = std::lower_bound(m_transitions_y.begin(), m_transitions_y.end(), y, std::greater_equal<>());
	return (it == m_transitions_y.end()) ? S16_MIN : *it;
}

BiomeGen *BiomeGenOriginal::clone(BiomeManager *biomemgr) const
{
	auto copy = new BiomeGenOriginal(biomemgr, m_params, m_csize);
#if IS_VOPI_ENGINE
	if (m_terrain_sampler)
		copy->setTerrainSampler(m_terrain_sampler->clone());
	copy->m_valleys_climate = m_valleys_climate;
	copy->m_climate_water_level = m_climate_water_level;
	copy->m_climate_altitude_chill = m_climate_altitude_chill;
	copy->m_climate_flags = m_climate_flags;
#endif
	return copy;
}

float BiomeGenOriginal::calcHeatAtPoint(v3s16 pos) const
{
	return NoiseFractal2D(&m_params->np_heat, pos.X, pos.Z, m_params->seed) +
		NoiseFractal2D(&m_params->np_heat_blend, pos.X, pos.Z, m_params->seed);
}

float BiomeGenOriginal::calcHumidityAtPoint(v3s16 pos) const
{
	return NoiseFractal2D(&m_params->np_humidity, pos.X, pos.Z, m_params->seed) +
		NoiseFractal2D(&m_params->np_humidity_blend, pos.X, pos.Z, m_params->seed);
}

Biome *BiomeGenOriginal::calcBiomeAtPoint(v3s16 pos) const
{
	return calcBiomeFromNoise(calcHeatAtPoint(pos), calcHumidityAtPoint(pos), pos);
}


void BiomeGenOriginal::calcBiomeNoise(v3s16 pmin)
{
	m_pmin = pmin;
#if IS_VOPI_ENGINE
	if (m_terrain_sampler)
		m_terrain_sampler->beginChunk();
#endif

	noise_heat->noiseMap2D(pmin.X, pmin.Z);
	noise_humidity->noiseMap2D(pmin.X, pmin.Z);
	noise_heat_blend->noiseMap2D(pmin.X, pmin.Z);
	noise_humidity_blend->noiseMap2D(pmin.X, pmin.Z);

	for (s32 i = 0; i < m_csize.X * m_csize.Z; i++) {
		noise_heat->result[i]     += noise_heat_blend->result[i];
		noise_humidity->result[i] += noise_humidity_blend->result[i];
	}
}


biome_t *BiomeGenOriginal::getBiomes(s16 *heightmap, v3s16 pmin)
{
	for (s16 zr = 0; zr < m_csize.Z; zr++)
	for (s16 xr = 0; xr < m_csize.X; xr++) {
		s32 i = zr * m_csize.X + xr;
		Biome *biome = calcBiomeFromNoise(
			noise_heat->result[i],
			noise_humidity->result[i],
			v3s16(pmin.X + xr, heightmap[i], pmin.Z + zr));

		biomemap[i] = biome->index;
	}

	return biomemap;
}


Biome *BiomeGenOriginal::getBiomeAtPoint(v3s16 pos) const
{
	return getBiomeAtIndex(
		(pos.Z - m_pmin.Z) * m_csize.X + (pos.X - m_pmin.X),
		pos);
}


Biome *BiomeGenOriginal::getBiomeAtIndex(size_t index, v3s16 pos) const
{
	return calcBiomeFromNoise(
		noise_heat->result[index],
		noise_humidity->result[index],
		pos);
}


#if IS_VOPI_ENGINE
Biome *BiomeGenOriginal::calcBiomeFromNoise(float heat, float humidity, v3s16 pos,
		const BiomeTerrainForm *form) const
#else
Biome *BiomeGenOriginal::calcBiomeFromNoise(float heat, float humidity, v3s16 pos) const
#endif
{
	Biome *biome_closest = nullptr;
	Biome *biome_closest_blend = nullptr;
#if IS_VOPI_ENGINE
	double dist_min = std::numeric_limits<double>::max();
	double dist_min_blend = std::numeric_limits<double>::max();
	// Priority outranks distance: a candidate replaces the current best when
	// its priority is higher, or equal with a smaller distance.
	s16 priority_closest = std::numeric_limits<s16>::min();
	s16 priority_blend = std::numeric_limits<s16>::min();
	auto outranks = [](const Biome *candidate, s16 best_priority,
			double dist, double best_dist) {
		return candidate->priority > best_priority ||
			(candidate->priority == best_priority && dist < best_dist);
	};
#else
	float dist_min = FLT_MAX;
	float dist_min_blend = FLT_MAX;
#endif
#if IS_VOPI_ENGINE
	BiomeTerrainForm sampled_form;
	bool form_sampled = form != nullptr;
	bool form_available = form != nullptr;
	if (form)
		sampled_form = *form;
#endif

	for (size_t i = 1; i < m_bmgr->getNumObjects(); i++) {
		Biome *b = (Biome *)m_bmgr->getRaw(i);
		if (!b ||
				pos.Y < b->min_pos.Y || pos.Y > b->max_pos.Y + b->vertical_blend ||
				pos.X < b->min_pos.X || pos.X > b->max_pos.X ||
				pos.Z < b->min_pos.Z || pos.Z > b->max_pos.Z)
			continue;

#if IS_VOPI_ENGINE
		if (b->hasClimateBounds() && !b->matchesClimate(heat, humidity))
			continue;
		if (b->hasFormConstraints()) {
			if (!form_sampled) {
				form_available = getBiomeForm(v2s16(pos.X, pos.Z), sampled_form);
				form_sampled = true;
			}
			if (!form_available || !b->matchesForm(sampled_form))
				continue;
		}
#endif
		float d_heat = heat - b->heat_point;
		float d_humidity = humidity - b->humidity_point;
		float dist = ((d_heat * d_heat) + (d_humidity * d_humidity));
		if (b->weight > 0.f)
		       dist /= b->weight;

#if IS_VOPI_ENGINE
		// Preserve ordinary float distances and their registration-order ties.
		// Finite float32 inputs can overflow in the squares or weight division,
		// but their weighted distance always fits in double precision.
		double selection_dist = dist;
		if (!std::isfinite(dist) && std::isfinite(b->weight)) {
			double heat_delta = static_cast<double>(heat) - b->heat_point;
			double humidity_delta = static_cast<double>(humidity) - b->humidity_point;
			selection_dist = heat_delta * heat_delta + humidity_delta * humidity_delta;
			if (b->weight > 0.f)
				selection_dist /= b->weight;
		}
#else
		const float selection_dist = dist;
#endif

#if IS_VOPI_ENGINE
		if (pos.Y <= b->max_pos.Y) { // Within y limits of biome b
			if (outranks(b, priority_closest, selection_dist, dist_min)) {
				priority_closest = b->priority;
				dist_min = selection_dist;
				biome_closest = b;
			}
		} else if (outranks(b, priority_blend, selection_dist, dist_min_blend)) {
			// Blend area above biome b
			priority_blend = b->priority;
			dist_min_blend = selection_dist;
			biome_closest_blend = b;
		}
#else
		if (pos.Y <= b->max_pos.Y) { // Within y limits of biome b
			if (selection_dist < dist_min) {
				dist_min = selection_dist;
				biome_closest = b;
			}
		} else if (selection_dist < dist_min_blend) { // Blend area above biome b
			dist_min_blend = selection_dist;
			biome_closest_blend = b;
		}
#endif
	}

	// Carefully tune pseudorandom seed variation to avoid single node dither
	// and create larger scale blending patterns similar to horizontal biome
	// blend.
	// The calculation can be a negative floating point number, which is an
	// undefined behavior if assigned to unsigned integer. Cast the result
	// into signed integer before it is casted into unsigned integer to
	// eliminate the undefined behavior.
	const u64 seed = static_cast<s64>(pos.Y + (heat + humidity) * 0.9f);
	PcgRandom rng(seed);

#if IS_VOPI_ENGINE
	// The blend candidate must outrank the in-range one, or tie it in
	// priority and be at least as close, before the dither is consulted.
	const bool blend_eligible = biome_closest_blend &&
		(priority_blend > priority_closest ||
		(priority_blend == priority_closest && dist_min_blend <= dist_min));
#else
	const bool blend_eligible = biome_closest_blend && dist_min_blend <= dist_min;
#endif
	if (blend_eligible &&
			rng.range(0, biome_closest_blend->vertical_blend) >=
			pos.Y - biome_closest_blend->max_pos.Y)
		return biome_closest_blend;

	return (biome_closest) ? biome_closest : (Biome *)m_bmgr->getRaw(BIOME_NONE);
}


////////////////////////////////////////////////////////////////////////////////

ObjDef *Biome::clone() const
{
	auto obj = new Biome();
	ObjDef::cloneTo(obj);
	NodeResolver::cloneTo(obj);

	obj->c_top = c_top;
	obj->c_filler = c_filler;
	obj->c_stone = c_stone;
	obj->c_water_top = c_water_top;
	obj->c_water = c_water;
	obj->c_river_water = c_river_water;
	obj->c_riverbed = c_riverbed;
	obj->c_dust = c_dust;
	obj->c_cave_liquid = c_cave_liquid;
	obj->c_dungeon = c_dungeon;
	obj->c_dungeon_alt = c_dungeon_alt;
	obj->c_dungeon_stair = c_dungeon_stair;

	obj->depth_top = depth_top;
	obj->depth_filler = depth_filler;
	obj->depth_water_top = depth_water_top;
	obj->depth_riverbed = depth_riverbed;
#if IS_VOPI_ENGINE
	obj->c_seabed = c_seabed;
	obj->depth_seabed = depth_seabed;
#endif

	obj->min_pos = min_pos;
	obj->max_pos = max_pos;
	obj->heat_point = heat_point;
	obj->humidity_point = humidity_point;
	obj->vertical_blend = vertical_blend;
	obj->weight = weight;
#if IS_VOPI_ENGINE
	obj->heat_min = heat_min;
	obj->heat_max = heat_max;
	obj->humidity_min = humidity_min;
	obj->humidity_max = humidity_max;
	obj->base_min = base_min;
	obj->base_max = base_max;
	obj->valley_depth_min = valley_depth_min;
	obj->valley_depth_max = valley_depth_max;
	obj->valley_pos_min = valley_pos_min;
	obj->valley_pos_max = valley_pos_max;
	obj->mountain_min = mountain_min;
	obj->mountain_max = mountain_max;
	obj->priority = priority;
#endif

	return obj;
}

#if IS_VOPI_ENGINE
bool Biome::hasClimateBounds() const
{
	return std::isfinite(heat_min) || std::isfinite(heat_max) ||
		std::isfinite(humidity_min) || std::isfinite(humidity_max);
}

bool Biome::matchesClimate(float heat, float humidity) const
{
	return heat >= heat_min && heat <= heat_max &&
		humidity >= humidity_min && humidity <= humidity_max;
}

bool Biome::hasFormConstraints() const
{
	return std::isfinite(base_min) || std::isfinite(base_max) ||
		valley_depth_min > 0.0f || std::isfinite(valley_depth_max) ||
		valley_pos_min > 0.0f || valley_pos_max < 1.0f ||
		mountain_min > 0.0f || std::isfinite(mountain_max);
}

bool Biome::matchesForm(const BiomeTerrainForm &form) const
{
	return form.base >= base_min && form.base <= base_max &&
		form.valley_depth >= valley_depth_min &&
		form.valley_depth <= valley_depth_max &&
		form.valley_pos >= valley_pos_min && form.valley_pos <= valley_pos_max &&
		form.mountain >= mountain_min && form.mountain <= mountain_max;
}

void BiomeGenOriginal::setTerrainSampler(std::unique_ptr<BiomeTerrainSampler> sampler)
{
	m_terrain_sampler = std::move(sampler);
}

void BiomeGenOriginal::setValleysClimate(const MapgenValleysParams &params)
{
	// Reconfiguration must never keep a context from different terrain params.
	setTerrainSampler(createValleysBiomeTerrainSampler(params));
	m_valleys_climate = true;
	m_climate_water_level = params.water_level;
	m_climate_altitude_chill = std::fmax(params.altitude_chill, 1.0f);
	m_climate_flags = params.spflags & (MGVALLEYS_ALT_CHILL |
		MGVALLEYS_ALT_DRY | MGVALLEYS_HUMID_RIVERS);
}

// The selector converts this float expression to s64. Check its range for
// every query Y before either the generation maps or the API can use it.
// Compare in double: float(s64::max) rounds up to the excluded upper bound.
static bool hasSafeBiomeSeed(float heat, float humidity)
{
	const float low = S16_MIN + (heat + humidity) * 0.9f;
	const float high = S16_MAX + (heat + humidity) * 0.9f;
	return std::isfinite(low) && std::isfinite(high) &&
		static_cast<double>(low) >= -0x1p63 &&
		static_cast<double>(high) < 0x1p63;
}

bool BiomeGenOriginal::sampleEffectiveClimate(v2s16 pos, EffectiveBiomeClimate &out,
		bool include_context) const
{
	if (!hasEffectiveClimate() || !m_terrain_sampler)
		return false;
	// Both generation and queries use scalar noise at the exact same X/Z.
	// Bulk noise has different rounding; it remains the legacy/river-depth input.
	const v3s16 point(pos.X, 0, pos.Y);
	EffectiveBiomeClimate result{};
	result.raw_heat = calcHeatAtPoint(point);
	result.raw_humidity = calcHumidityAtPoint(point);
	if (!std::isfinite(result.raw_heat) || !std::isfinite(result.raw_humidity))
		return false;
	BiomeClimateContext context{};
	if ((include_context || m_climate_flags) &&
			!m_terrain_sampler->sampleClimate(pos, context))
		return false;
	result.river_bank_height = context.river_bank_height;
	result.climate_reference_height = std::fmax(context.river_bank_height,
		static_cast<float>(context.column_max_y));
	result.form = context.form;
	const auto climate = calcValleysClimate(result.raw_heat, result.raw_humidity,
		context.river_bank_height, context.column_max_y, m_climate_water_level,
		m_climate_altitude_chill, m_climate_flags);
	result.heat = climate.heat;
	result.humidity = climate.humidity;
	if (!std::isfinite(result.heat) || !std::isfinite(result.humidity) ||
			!std::isfinite(result.river_bank_height) ||
			!std::isfinite(result.climate_reference_height) ||
			!hasSafeBiomeSeed(result.heat, result.humidity))
		return false;
	out = result;
	return true;
}

bool BiomeGenOriginal::getEffectiveClimate(v2s16 pos, EffectiveBiomeClimate &out) const
{
	return sampleEffectiveClimate(pos, out, true);
}

bool BiomeGenOriginal::getEffectiveClimate(v2s16 pos, ValleysClimate &out) const
{
	EffectiveBiomeClimate climate;
	if (!sampleEffectiveClimate(pos, climate, false))
		return false;
	out = {climate.heat, climate.humidity};
	return true;
}

bool BiomeGenOriginal::getEffectiveBiomeData(v3s16 pos, EffectiveBiomeData &out) const
{
	if (!getEffectiveClimate(v2s16(pos.X, pos.Z), out))
		return false;
	Biome *biome = calcBiomeFromNoise(out.heat, out.humidity, pos, &out.form);
	if (!biome || biome->index == OBJDEF_INVALID_INDEX)
		return false;
	out.biome = biome->index;
	return true;
}

bool BiomeGenOriginal::getBiomeForm(v2s16 pos, BiomeTerrainForm &form) const
{
	if (!m_terrain_sampler)
		return false;
	BiomeClimateContext context;
	if (!m_terrain_sampler->sampleClimate(pos, context))
		return false;
	form = context.form;
	return true;
}

bool BiomeGenOriginal::getBiomeTerrainHeight(v2s16 pos, float &height) const
{
	if (!m_terrain_sampler)
		return false;
	height = m_terrain_sampler->sampleHeight(pos);
	return std::isfinite(height);
}
#endif

void Biome::resolveNodeNames()
{
	getIdFromNrBacklog(&c_top,           "mapgen_stone",              CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_filler,        "mapgen_stone",              CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_stone,         "mapgen_stone",              CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_water_top,     "mapgen_water_source",       CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_water,         "mapgen_water_source",       CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_river_water,   "mapgen_river_water_source", CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_riverbed,      "mapgen_stone",              CONTENT_AIR,    false);
	getIdFromNrBacklog(&c_dust,          "ignore",                    CONTENT_IGNORE, false);
	getIdsFromNrBacklog(&c_cave_liquid);
	getIdFromNrBacklog(&c_dungeon,       "ignore",                    CONTENT_IGNORE, false);
	getIdFromNrBacklog(&c_dungeon_alt,   "ignore",                    CONTENT_IGNORE, false);
	getIdFromNrBacklog(&c_dungeon_stair, "ignore",                    CONTENT_IGNORE, false);
#if IS_VOPI_ENGINE
	getIdFromNrBacklog(&c_seabed,        "ignore",                    CONTENT_IGNORE, false);
#endif
}
