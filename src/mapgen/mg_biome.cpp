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
#include "profiler.h"
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
	b->m_nodenames.emplace_back("ignore");
	b->m_nodenames.emplace_back("ignore");
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
#if IS_VOPI_ENGINE
	settings->getNoiseParams("mg_biome_np_base_blend",     np_base_blend);
	settings->getNoiseParams("mg_biome_np_valley_depth_blend", np_valley_depth_blend);
	settings->getNoiseParams("mg_biome_np_variant",        np_variant);
	settings->getNoiseParams("mg_biome_np_shift",          np_shift);
	settings->getNoiseParams("mg_biome_np_top_patch",      np_top_patch);
#endif
}


void BiomeParamsOriginal::writeParams(Settings *settings) const
{
	settings->setNoiseParams("mg_biome_np_heat",           np_heat);
	settings->setNoiseParams("mg_biome_np_heat_blend",     np_heat_blend);
	settings->setNoiseParams("mg_biome_np_humidity",       np_humidity);
	settings->setNoiseParams("mg_biome_np_humidity_blend", np_humidity_blend);
#if IS_VOPI_ENGINE
	settings->setNoiseParams("mg_biome_np_base_blend",     np_base_blend);
	settings->setNoiseParams("mg_biome_np_valley_depth_blend", np_valley_depth_blend);
	settings->setNoiseParams("mg_biome_np_variant",        np_variant);
	settings->setNoiseParams("mg_biome_np_shift",          np_shift);
	settings->setNoiseParams("mg_biome_np_top_patch",      np_top_patch);
#endif
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
#if IS_VOPI_ENGINE
	return heatAt(shiftedColumn(v2s16(pos.X, pos.Z)));
#else
	return NoiseFractal2D(&m_params->np_heat, pos.X, pos.Z, m_params->seed) +
		NoiseFractal2D(&m_params->np_heat_blend, pos.X, pos.Z, m_params->seed);
#endif
}

float BiomeGenOriginal::calcHumidityAtPoint(v3s16 pos) const
{
#if IS_VOPI_ENGINE
	return humidityAt(shiftedColumn(v2s16(pos.X, pos.Z)));
#else
	return NoiseFractal2D(&m_params->np_humidity, pos.X, pos.Z, m_params->seed) +
		NoiseFractal2D(&m_params->np_humidity_blend, pos.X, pos.Z, m_params->seed);
#endif
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

#if IS_VOPI_ENGINE
	m_shift_map.clear();
	if (hasShift() || m_valleys_climate) {
		// The scalar climate of every column of the chunk, once: at the
		// displaced point with a shift noise, at the column itself without
		// one. Bulk noise cannot be read at a displaced point, and a mapgen
		// with the effective climate selects and answers queries by the
		// scalar noise, so its maps hold the scalar raw climate from here
		// on and its terrain pass corrects them in place where its flags
		// say, without reading the noise a second time.
		const bool shift = hasShift();
		if (shift)
			m_shift_map.resize(m_csize.X * m_csize.Z);
		for (s16 zr = 0; zr < m_csize.Z; zr++)
		for (s16 xr = 0; xr < m_csize.X; xr++) {
			const s32 i = zr * m_csize.X + xr;
			const v2s16 column(pmin.X + xr, pmin.Z + zr);
			const v2f at = shift ? displace(column) : v2f(column.X, column.Y);
			if (shift)
				m_shift_map[i] = at;
			noise_heat->result[i] = heatAt(at);
			noise_humidity->result[i] = humidityAt(at);
		}
		return;
	}
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
		const BiomeTerrainForm *form, const float *known_variant) const
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
	m_profile_selections++;
	BiomeTerrainForm sampled_form;
	bool form_sampled = form != nullptr;
	bool form_available = form != nullptr;
	if (form)
		sampled_form = *form;
	float variant = known_variant ? *known_variant : 0.0f;
	bool variant_sampled = known_variant != nullptr;
	// The displaced column the variant is read at, found once, when the
	// first candidate needs it. The form is the column's own: a bound on
	// the relief must hold where the relief stands, or a wetland biome
	// climbs the cliff beside its flat and a flank biome lies in the flat.
	v2s16 read_column;
	bool read_column_known = false;
	auto readColumn = [&]() {
		if (!read_column_known) {
			read_column = nearestColumn(shiftedColumn(v2s16(pos.X, pos.Z)));
			read_column_known = true;
		}
		return read_column;
	};
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
		if (b->hasVariantBounds()) {
			if (!variant_sampled) {
				variant = variantAt(readColumn());
				variant_sampled = true;
			}
			if (!b->matchesVariant(variant))
				continue;
		}
		if (b->hasFormConstraints()) {
			if (!form_sampled) {
				form_available = formAt(v2s16(pos.X, pos.Z), sampled_form);
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
	obj->c_waterline = c_waterline;
	obj->c_top_patch = c_top_patch;
	obj->c_top_patch_alt = c_top_patch_alt;
	obj->top_patch_min = top_patch_min;
	obj->top_patch_alt_max = top_patch_alt_max;
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
	obj->variant_min = variant_min;
	obj->variant_max = variant_max;
	obj->base_min = base_min;
	obj->base_max = base_max;
	obj->valley_depth_min = valley_depth_min;
	obj->valley_depth_max = valley_depth_max;
	obj->valley_pos_min = valley_pos_min;
	obj->valley_pos_max = valley_pos_max;
	obj->mountain_min = mountain_min;
	obj->mountain_max = mountain_max;
	obj->body_min = body_min;
	obj->body_max = body_max;
	obj->wetland_min = wetland_min;
	obj->wetland_max = wetland_max;
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

bool Biome::hasVariantBounds() const
{
	return std::isfinite(variant_min) || std::isfinite(variant_max);
}

bool Biome::matchesVariant(float variant) const
{
	return variant >= variant_min && variant <= variant_max;
}

bool Biome::hasFormConstraints() const
{
	return std::isfinite(base_min) || std::isfinite(base_max) ||
		valley_depth_min > 0.0f || std::isfinite(valley_depth_max) ||
		valley_pos_min > 0.0f || valley_pos_max < 1.0f ||
		mountain_min > 0.0f || std::isfinite(mountain_max) ||
		body_min > 0.0f || std::isfinite(body_max) ||
		wetland_min > 0.0f || wetland_max < 1.0f;
}

bool Biome::matchesForm(const BiomeTerrainForm &form) const
{
	return form.base >= base_min && form.base <= base_max &&
		form.valley_depth >= valley_depth_min &&
		form.valley_depth <= valley_depth_max &&
		form.valley_pos >= valley_pos_min && form.valley_pos <= valley_pos_max &&
		form.mountain >= mountain_min && form.mountain <= mountain_max &&
		form.body >= body_min && form.body <= body_max &&
		form.wetland >= wetland_min && form.wetland <= wetland_max;
}

void BiomeGenOriginal::setTerrainSampler(std::unique_ptr<BiomeTerrainSampler> sampler)
{
	m_terrain_sampler = std::move(sampler);
}

void BiomeGenOriginal::setTerrainChunk(const BiomeTerrainChunk *chunk)
{
	if (m_terrain_sampler)
		m_terrain_sampler->setChunk(chunk);
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

bool BiomeGenOriginal::isSelectableClimate(float heat, float humidity)
{
	return std::isfinite(heat) && std::isfinite(humidity) &&
		hasSafeBiomeSeed(heat, humidity);
}

bool BiomeGenOriginal::sampleEffectiveClimate(v2s16 pos, EffectiveBiomeClimate &out,
		bool include_context) const
{
	if (!hasEffectiveClimate() || !m_terrain_sampler)
		return false;
	// Both generation and queries use scalar noise at the exact same X/Z.
	// Bulk noise has different rounding; it remains the legacy/river-depth input.
	const v2f at = shiftedColumn(pos);
	EffectiveBiomeClimate result{};
	result.raw_heat = heatAt(at);
	result.raw_humidity = humidityAt(at);
	if (!std::isfinite(result.raw_heat) || !std::isfinite(result.raw_humidity))
		return false;
	// The bank, the surface and the form are the column's own: the
	// corrections, the reference height and the relief describe the
	// place, not the displaced point.
	BiomeClimateContext context{};
	if ((include_context || m_climate_flags) &&
			!m_terrain_sampler->sampleClimate(pos, context))
		return false;
	result.river_bank_height = context.river_bank_height;
	result.climate_reference_height = std::fmax(context.river_bank_height,
		static_cast<float>(context.column_max_y));
	result.form = context.form;
	result.variant = 0.0f;
	if (include_context) {
		blendForm(pos, result.form);
		result.variant = variantAt(nearestColumn(at));
	}
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
	Biome *biome = calcBiomeFromNoise(out.heat, out.humidity, pos, &out.form,
		&out.variant);
	if (!biome || biome->index == OBJDEF_INVALID_INDEX)
		return false;
	out.biome = biome->index;
	return true;
}

bool BiomeGenOriginal::getBiomeForm(v2s16 pos, BiomeTerrainForm &form) const
{
	return formAt(pos, form);
}

bool BiomeGenOriginal::hasShift() const
{
	const NoiseParams &np = m_params->np_shift;
	return np.scale != 0.0f || np.offset != 0.0f;
}

// The Z displacement reads the shift noise under another seed, so that
// the two are independent fields rather than one field and its copy.
static constexpr s32 SHIFT_Z_SEED = 1013;

v2f BiomeGenOriginal::displace(v2s16 pos) const
{
	v2f at(pos.X, pos.Y);
	const NoiseParams &np = m_params->np_shift;
	at.X += NoiseFractal2D(&np, pos.X, pos.Y, m_params->seed);
	at.Y += NoiseFractal2D(&np, pos.X, pos.Y, m_params->seed + SHIFT_Z_SEED);
	return at;
}

v2f BiomeGenOriginal::shiftedColumn(v2s16 pos) const
{
	if (!hasShift())
		return v2f(pos.X, pos.Y);
	const s32 xr = pos.X - m_pmin.X, zr = pos.Y - m_pmin.Z;
	if (!m_shift_map.empty() && xr >= 0 && xr < m_csize.X && zr >= 0 && zr < m_csize.Z)
		return m_shift_map[zr * m_csize.X + xr];
	return displace(pos);
}

v2s16 BiomeGenOriginal::nearestColumn(v2f at)
{
	// A point displaced past the edge of the world reads its last column:
	// the conversion never wraps.
	constexpr long limit = MAX_MAP_GENERATION_LIMIT;
	return v2s16(static_cast<s16>(std::clamp(std::lround(at.X), -limit, limit)),
		static_cast<s16>(std::clamp(std::lround(at.Y), -limit, limit)));
}

float BiomeGenOriginal::heatAt(v2f at) const
{
	return NoiseFractal2D(&m_params->np_heat, at.X, at.Y, m_params->seed) +
		NoiseFractal2D(&m_params->np_heat_blend, at.X, at.Y, m_params->seed);
}

float BiomeGenOriginal::humidityAt(v2f at) const
{
	return NoiseFractal2D(&m_params->np_humidity, at.X, at.Y, m_params->seed) +
		NoiseFractal2D(&m_params->np_humidity_blend, at.X, at.Y, m_params->seed);
}

bool BiomeGenOriginal::formAt(v2s16 at, BiomeTerrainForm &form) const
{
	if (!m_terrain_sampler)
		return false;
	m_profile_form_reads++;
	BiomeClimateContext context;
	if (!m_terrain_sampler->sampleClimate(at, context))
		return false;
	form = context.form;
	blendForm(at, form);
	return true;
}

float BiomeGenOriginal::variantAt(v2s16 at) const
{
	const NoiseParams &np = m_params->np_variant;
	if (np.scale == 0.0f)
		return np.offset;
	m_profile_variant_reads++;
	return NoiseFractal2D(&np, at.X, at.Y, m_params->seed);
}

void BiomeGenOriginal::blendForm(v2s16 pos, BiomeTerrainForm &form) const
{
	auto active = [](const NoiseParams &np) {
		return np.scale != 0.0f || np.offset != 0.0f;
	};
	if (active(m_params->np_base_blend))
		form.base += NoiseFractal2D(&m_params->np_base_blend, pos.X, pos.Y,
			m_params->seed);
	if (active(m_params->np_valley_depth_blend))
		form.valley_depth = std::fmax(0.0f, form.valley_depth + NoiseFractal2D(
			&m_params->np_valley_depth_blend, pos.X, pos.Y, m_params->seed));
}

content_t BiomeGen::getTopNode(const Biome *biome, v2s16 column) const
{
	return biome->c_top;
}

content_t BiomeGenOriginal::topNodeFor(const Biome &biome, float patch_noise)
{
	if (biome.c_top_patch != CONTENT_IGNORE && patch_noise >= biome.top_patch_min)
		return biome.c_top_patch;
	if (biome.c_top_patch_alt != CONTENT_IGNORE &&
			patch_noise <= biome.top_patch_alt_max)
		return biome.c_top_patch_alt;
	return biome.c_top;
}

content_t BiomeGenOriginal::getTopNode(const Biome *biome, v2s16 column) const
{
	// The noise is read only where a biome lays patches, so a world
	// without them pays nothing for the field.
	if (biome->c_top_patch == CONTENT_IGNORE &&
			biome->c_top_patch_alt == CONTENT_IGNORE)
		return biome->c_top;
	return topNodeFor(*biome, NoiseFractal2D(&m_params->np_top_patch,
		column.X, column.Y, m_params->seed));
}

bool BiomeGenOriginal::getBiomeTerrainHeight(v2s16 pos, float &height) const
{
	if (!m_terrain_sampler)
		return false;
	height = m_terrain_sampler->sampleHeight(pos);
	return std::isfinite(height);
}

void BiomeGenOriginal::resetProfile() const
{
	m_profile_selections = 0;
	m_profile_form_reads = 0;
	m_profile_variant_reads = 0;
	if (m_terrain_sampler)
		m_terrain_sampler->resetProfile();
}

void BiomeGenOriginal::profileChunk() const
{
	g_profiler->avg("Biomes: selections [#]", m_profile_selections);
	g_profiler->avg("Biomes: form reads [#]", m_profile_form_reads);
	g_profiler->avg("Biomes: variant reads [#]", m_profile_variant_reads);
	if (m_terrain_sampler)
		m_terrain_sampler->profileChunk();
	resetProfile();
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
	getIdFromNrBacklog(&c_waterline,     "ignore",                    CONTENT_IGNORE, false);
	getIdFromNrBacklog(&c_top_patch,     "ignore",                    CONTENT_IGNORE, false);
	getIdFromNrBacklog(&c_top_patch_alt, "ignore",                    CONTENT_IGNORE, false);
#endif
}
