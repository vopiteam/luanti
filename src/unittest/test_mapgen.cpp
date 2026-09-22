// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 Minetest core developers & community

#include "test.h"

#include "emerge.h"
#include "mapgen/mapgen.h"
#include "mapgen/mg_biome.h"
#include "irrlicht_changes/printing.h"
#include "mock_server.h"

#if IS_VOPI_ENGINE
#include "mapgen/mapgen_valleys.h"
#include "mapgen/mg_biome_terrain.h"
#include "mapgen/mg_decoration.h"
#include "script/common/c_types.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <limits>
extern "C" {
#include <lauxlib.h>
}
Biome *read_biome_def(lua_State *L, int index, const NodeDefManager *ndef);
#endif

class TestMapgen : public TestBase
{
public:
	TestMapgen() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestMapgen"; }

	void runTests(IGameDef *gamedef);

	void testBiomeGen(IGameDef *gamedef);
	void testMapgenEdges();
#if IS_VOPI_ENGINE
	void testBiomeFormRanges();
	void testBiomeFormParsing();
	void testBiomeFormSelection();
	void testBiomePriority();
	void testBiomeNumericDistances(IGameDef *gamedef);
	void testBiomeNumericBlending(IGameDef *gamedef);
	void testBiomeFormClone(IGameDef *gamedef);
	void testBiomeTerrainValleys();
	void testBiomeTerrainProfile();
	void testBiomeTerrainFloaterSeeds();
	void testValleysSurfaceModel(IGameDef *gamedef);
	void testValleysClimateParams();
	void testValleysClimateCorrections();
	void testValleysClimateContext();
	void testEffectiveBiomeSelection();
	void testEffectiveClimateSafety();
	void testEffectiveClimateContextDemand();
	void testValleysEffectiveClimate(IGameDef *gamedef);
	void testValleysFormGeneration(IGameDef *gamedef);
	void testValleysFloaterBiomes(IGameDef *gamedef);
	void testDecorationBiomeAtSurface(IGameDef *gamedef);
#endif
};

static TestMapgen g_test_instance;

namespace {
	class MockBiomeManager : public BiomeManager {
	public:
		MockBiomeManager(Server *server) : BiomeManager(server) {}

		void setNodeDefManager(const NodeDefManager *ndef)
		{
			m_ndef = ndef;
		}
	};
}

void TestMapgen::runTests(IGameDef *gamedef)
{
	TEST(testBiomeGen, gamedef);
	TEST(testMapgenEdges);
#if IS_VOPI_ENGINE
	TEST(testBiomeFormRanges);
	TEST(testBiomeFormParsing);
	TEST(testBiomeFormSelection);
	TEST(testBiomePriority);
	TEST(testBiomeNumericDistances, gamedef);
	TEST(testBiomeNumericBlending, gamedef);
	TEST(testBiomeFormClone, gamedef);
	TEST(testBiomeTerrainValleys);
	TEST(testBiomeTerrainProfile);
	TEST(testBiomeTerrainFloaterSeeds);
	TEST(testValleysSurfaceModel, gamedef);
	TEST(testValleysClimateParams);
	TEST(testValleysClimateCorrections);
	TEST(testValleysClimateContext);
	TEST(testEffectiveBiomeSelection);
	TEST(testEffectiveClimateSafety);
	TEST(testEffectiveClimateContextDemand);
	TEST(testValleysEffectiveClimate, gamedef);
	TEST(testValleysFormGeneration, gamedef);
	TEST(testValleysFloaterBiomes, gamedef);
	TEST(testDecorationBiomeAtSurface, gamedef);
#endif
}

void TestMapgen::testBiomeGen(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager bmgr(&server);
	bmgr.setNodeDefManager(gamedef->getNodeDefManager());

	{
		// Add some biomes (equivalent to l_register_biome)
		// Taken from minetest_game/mods/default/mapgen.lua
		size_t bmgr_count = bmgr.getNumObjects(); // this is 1 ?

		Biome *b = BiomeManager::create(BIOMETYPE_NORMAL);
		b->name = "deciduous_forest";
		b->c_top = t_CONTENT_GRASS;
		b->depth_top = 1;
		b->c_filler = t_CONTENT_BRICK; // dirt
		b->depth_filler = 3;
		b->c_stone = t_CONTENT_STONE;
		b->min_pos.Y = 1;
		b->heat_point = 60.0f;
		b->humidity_point = 68.0f;
		UASSERT(bmgr.add(b) != OBJDEF_INVALID_HANDLE);

		b = BiomeManager::create(BIOMETYPE_NORMAL);
		b->name = "deciduous_forest_shore";
		b->c_top = t_CONTENT_BRICK; // dirt
		b->depth_top = 1;
		b->c_filler = t_CONTENT_BRICK; // dirt
		b->depth_filler = 3;
		b->c_stone = t_CONTENT_STONE;
		b->max_pos.Y = 0;
		b->heat_point = 60.0f;
		b->humidity_point = 68.0f;
		UASSERT(bmgr.add(b) != OBJDEF_INVALID_HANDLE);
		UASSERT(bmgr.getNumObjects() - bmgr_count == 2);
	}


	std::unique_ptr<BiomeParams> params(BiomeManager::createBiomeParams(BIOMEGEN_ORIGINAL));

	constexpr v3s16 CSIZE(16, 16, 16); // misleading name. measured in nodes.
	std::unique_ptr<BiomeGen> biomegen(
		bmgr.createBiomeGen(BIOMEGEN_ORIGINAL, params.get(), CSIZE)
	);

	{
		// Test biome transitions
		//   getBiomeAtIndex (Y only)
		//   getNextTransitionY
		const struct {
			s16 check_y;
			const char *name;
			s16 next_y;
		} expected_biomes[] = {
			{ MAX_MAP_GENERATION_LIMIT, "deciduous_forest", 0 },
			{ 1, "deciduous_forest", 0 },
			{    0, "deciduous_forest_shore", S16_MIN },
			{ -100, "deciduous_forest_shore", S16_MIN },
		};
		for (const auto &expected : expected_biomes) {
			Biome *biome = biomegen->getBiomeAtIndex(
				(1 * CSIZE.X) + 1, // index in CSIZE 2D noise map
				v3s16(2000, expected.check_y, -1000) // absolute coordinates
			);
			s16 next_y = biomegen->getNextTransitionY(expected.check_y);

			//UASSERTEQ(auto, biome->name, expected.name);
			//UASSERTEQ(auto, next_y, expected.next_y);
			if (biome->name != expected.name) {
				errorstream << "FIXME " << FUNCTION_NAME << " " << biome->name
					<< " != " << expected.name << "\nThe test would have failed."
					<< std::endl;
				return;
			}
			if (next_y != expected.next_y) {
				errorstream << "FIXME " << FUNCTION_NAME << " " << next_y
					<< " != " << expected.next_y << "\nThe test would have failed."
					<< std::endl;
				return;
			}
		}
	}
}

void TestMapgen::testMapgenEdges()
{
	v3s16 emin, emax;

	std::tie(emin, emax) = get_mapgen_edges(31007, v3s16(5));
	UASSERTEQ(auto, emin, v3s16(-30912));
	UASSERTEQ(auto, emax, v3s16(30927));

	std::tie(emin, emax) = get_mapgen_edges(502 * MAP_BLOCKSIZE, v3s16(1, 2, 1));
	UASSERTEQ(auto, emin, v3s16(-8016));
	UASSERTEQ(auto, emax, v3s16(8031, 8015, 8031));
}

#if IS_VOPI_ENGINE
namespace {
// A sampler with a fixed height and, when given one, a fixed column form.
class TestTerrainSampler final : public BiomeTerrainSampler {
public:
	explicit TestTerrainSampler(float height) : height(height) {}
	TestTerrainSampler(float height, const BiomeTerrainForm &form) :
		height(height), form(form), has_form(true) {}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return has_form ? std::make_unique<TestTerrainSampler>(height, form) :
			std::make_unique<TestTerrainSampler>(height);
	}

	float sampleHeight(v2s16 pos) const override
	{
		++calls;
		last_pos = pos;
		return height;
	}

	bool sampleClimate(v2s16 pos, BiomeClimateContext &out) const override
	{
		if (!has_form)
			return false;
		++climate_calls;
		last_pos = pos;
		out = BiomeClimateContext{0.0f, 0, form};
		return true;
	}

	float height;
	BiomeTerrainForm form;
	bool has_form = false;
	mutable unsigned int calls = 0;
	mutable unsigned int climate_calls = 0;
	mutable v2s16 last_pos;
};

class CountingClimateSampler final : public BiomeTerrainSampler {
public:
	explicit CountingClimateSampler(std::unique_ptr<BiomeTerrainSampler> sampler) :
		m_sampler(std::move(sampler)) {}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::make_unique<CountingClimateSampler>(m_sampler->clone());
	}

	float sampleHeight(v2s16 pos) const override { return m_sampler->sampleHeight(pos); }

	bool sampleClimate(v2s16 pos, BiomeClimateContext &out) const override
	{
		++climate_calls;
		return m_sampler->sampleClimate(pos, out);
	}

	mutable unsigned int climate_calls = 0;

private:
	std::unique_ptr<BiomeTerrainSampler> m_sampler;
};

Biome *addTerrainTestBiome(BiomeManager &manager, const char *name,
		float heat, float humidity)
{
	auto biome = new Biome;
	biome->name = name;
	biome->heat_point = heat;
	biome->humidity_point = humidity;
	UASSERT(manager.add(biome) != OBJDEF_INVALID_HANDLE);
	return biome;
}

class MapgenTestVManip final : public MMVManip {
public:
	explicit MapgenTestVManip(const VoxelArea &area)
	{
		addArea(area);
		std::fill(m_data, m_data + m_area.getVolume(), MapNode(CONTENT_IGNORE));
		std::fill(m_flags, m_flags + m_area.getVolume(), 0);
	}
};
}

void TestMapgen::testValleysClimateParams()
{
	const struct { u16 input, expected; } cases[] = {
		{0, 1}, {1, 1}, {90, 90}, {100, 100}, {65535, 65535},
	};
	for (const auto &test : cases) {
		Settings raw;
		raw.setU16("mgvalleys_altitude_chill", test.input);
		MapgenValleysParams parsed;
		parsed.readParams(&raw);
		UASSERTEQ(u16, parsed.altitude_chill, test.expected);
		UASSERTEQ(u16, raw.getU16("mgvalleys_altitude_chill"), test.input);

		Settings saved;
		parsed.writeParams(&saved);
		UASSERTEQ(u16, saved.getU16("mgvalleys_altitude_chill"), test.expected);
		MapgenValleysParams restored;
		restored.readParams(&saved);
		UASSERTEQ(u16, restored.altitude_chill, test.expected);

		// Direct parameter construction can bypass readParams.
		MapgenValleysParams direct;
		direct.altitude_chill = test.input;
		direct.writeParams(&saved);
		UASSERTEQ(u16, saved.getU16("mgvalleys_altitude_chill"), test.expected);
		UASSERTEQ(u16, direct.altitude_chill, test.input);
	}
}

void TestMapgen::testValleysClimateCorrections()
{
	constexpr u32 all_climate = MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_ALT_DRY;
	constexpr u32 other_flags = MGVALLEYS_VARY_RIVER_DEPTH | MGVALLEYS_SEA_LEVEL_RIVERS |
		MGVALLEYS_CARVE_CLIFFS | MGVALLEYS_REMOVE_FLOATERS | MGVALLEYS_MOUNTAINS;
	const struct {
		const char *name;
		float raw_heat;
		float raw_humidity;
		float base;
		s16 column_max_y;
		int water_level;
		float altitude_chill;
		u32 flags;
		float heat;
		float humidity;
	} cases[] = {
		{"disabled", 30, 40, 10, 18, 10, 0, 0, 30, 40},
		{"unrelated flags", 30, 40, 10, 18, 10, 0, other_flags, 30, 40},
		{"below sea level", 30, 40, 8, 8, 10, 20, all_climate, 35, 48},
		{"at sea level", 30, 40, 10, 10, 10, 20, all_climate, 35, 48},
		{"relative altitude", 30, 40, 10, 18, 10, 20, all_climate, 27, 36},
		{"fractional bank", 30, 40, 10.5f, 10, 10, 20, all_climate, 34.5f, 47.75f},
		{"negative sea level", 30, 40, -10, -2, -10, 20, all_climate, 27, 36},
		{"below negative sea level", 30, 40, -14, -14, -10, 20, all_climate, 35, 48},
		{"river minimum depth", 30, 40, 10, 12, 10, 20, MGVALLEYS_HUMID_RIVERS, 30, 48},
		{"river depth boundary", 30, 40, 10, 14, 10, 20, MGVALLEYS_HUMID_RIVERS, 30, 48},
		{"river depth falloff", 30, 40, 10, 18, 10, 20, MGVALLEYS_HUMID_RIVERS, 30, 40},
		{"chill only", 30, 40, 10, 18, 10, 20, MGVALLEYS_ALT_CHILL, 27, 40},
		{"dryness only", 30, 40, 10, 18, 10, 20, MGVALLEYS_ALT_DRY, 30, 36},
		{"unclamped heat", 150, -10, 10, 18, 10, 20, all_climate, 147, -14},
		{"unclamped humidity", -30, 140, 10, 18, 10, 20, all_climate, -33, 136},
		{"zero divisor", 30, 40, 10, 18, 10, 0, all_climate, -125, -40},
		{"minimum divisor", 30, 40, 10, 18, 10, 1, all_climate, -125, -40},
		{"zero chill only", 30, 40, 10, 18, 10, 0, MGVALLEYS_ALT_CHILL, -125, 40},
		{"zero dryness only", 30, 40, 10, 18, 10, 0, MGVALLEYS_ALT_DRY, 30, -40},
		{"zero river only", 30, 40, 10, 18, 10, 0, MGVALLEYS_HUMID_RIVERS, 30, 40},
		{"zero below sea level", 30, 40, 8, 8, 10, 0, all_climate, 35, 48},
		{"zero at sea level", 30, 40, 10, 10, 10, 0, all_climate, 35, 48},
	};
	for (const auto &test : cases) {
		infostream << "Valleys climate correction: " << test.name << std::endl;
		const ValleysClimate climate = calcValleysClimate(test.raw_heat,
			test.raw_humidity, test.base, test.column_max_y, test.water_level,
			test.altitude_chill, test.flags);
		UASSERTEQ(float, climate.heat, test.heat);
		UASSERTEQ(float, climate.humidity, test.humidity);
	}
}

void TestMapgen::testValleysClimateContext()
{
	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	auto check = [](const BiomeTerrainSampler &sampler, v2s16 pos,
			float bank, s16 height) {
		BiomeClimateContext context;
		UASSERT(sampler.sampleClimate(pos, context));
		UASSERTEQ(float, context.river_bank_height, bank);
		UASSERTEQ(s16, context.column_max_y, height);
	};
	TestTerrainSampler unsupported(17.0f);
	BiomeClimateContext unavailable{8, 9, {}};
	UASSERT(!unsupported.sampleClimate(v2s16(0), unavailable));
	UASSERTEQ(float, unavailable.river_bank_height, 8.0f);
	UASSERTEQ(s16, unavailable.column_max_y, 9);

	MapgenValleysParams params;
	params.seed = 12345;
	params.water_level = 0;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	const v2s16 positions[] = {
		{-81, 79}, {-80, 80}, {-17, 31}, {-1, 0}, {0, -1}, {79, -80},
		{-MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT},
		{MAX_MAP_GENERATION_LIMIT, -MAX_MAP_GENERATION_LIMIT},
	};
	// These later terrain operations and chunk boundaries do not belong to
	// climate. The column has bank 16 and a 2D surface at 32; the density
	// relief reaching 52 above it is not the climate height, and the form
	// records a full-depth valley column on its ridge.
	for (u32 flags : {0U, u32(MGVALLEYS_CARVE_CLIFFS),
			u32(MGVALLEYS_REMOVE_FLOATERS),
			u32(MGVALLEYS_CARVE_CLIFFS | MGVALLEYS_REMOVE_FLOATERS)}) {
		params.spflags = flags;
		for (s16 floor : {s16(-MAX_MAP_GENERATION_LIMIT), s16(100)}) {
			params.floor_y = floor;
			params.chunksize = floor == 100 ? v3s16(9) : v3s16(1);
			params.mapgen_limit = floor == 100 ? 32 : MAX_MAP_GENERATION_LIMIT;
			auto sampler = createValleysBiomeTerrainSampler(params);
			for (auto pos : positions) {
				check(*sampler, pos, 16.0f, 32);
				BiomeClimateContext context;
				UASSERT(sampler->sampleClimate(pos, context));
				UASSERTEQ(float, context.form.base, 16.0f);
				UASSERTEQ(float, context.form.valley_depth, 16.0f);
				UASSERTEQ(float, context.form.valley_pos, 1.0f);
				UASSERTEQ(float, context.form.mountain, 0.0f);
			}
			if (floor == 100)
				UASSERTEQ(float, sampler->sampleHeight(v2s16(0)), 100.0f);
		}
	}

	params.spflags = 0;
	params.floor_y = -MAX_MAP_GENERATION_LIMIT;
	params.mapgen_limit = MAX_MAP_GENERATION_LIMIT;
	params.np_valley_depth = constant_noise(0.0f);
	params.np_inter_valley_slope = constant_noise(0.0f);
	// Initial height uses truncation, not floor or rounding. Extreme finite
	// surfaces are bounded before conversion to the node-coordinate type.
	const struct { float surface; s16 height; } flat_cases[] = {
		{-1.25f, -1}, {-0.75f, 0}, {0.75f, 0}, {1.25f, 1},
		{-1.0e9f, -MAX_MAP_GENERATION_LIMIT}, {1.0e9f, MAX_MAP_GENERATION_LIMIT},
	};
	for (const auto &test : flat_cases) {
		params.np_terrain_height = constant_noise(test.surface);
		check(*createValleysBiomeTerrainSampler(params), v2s16(-1, 1),
			test.surface, test.height);
	}
	// The finite 2D surface already reaches the ceiling. Its upper bound
	// would overflow (3e38 + 16e37), but no density search is necessary.
	params.np_terrain_height = constant_noise(3.0e38f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_inter_valley_slope = constant_noise(1.0e37f);
	params.np_inter_valley_fill = constant_noise(1.0f);
	auto ceiling = createValleysBiomeTerrainSampler(params);
	check(*ceiling, v2s16(-1, 1), 3.0e38f, MAX_MAP_GENERATION_LIMIT);
	check(*ceiling, v2s16(-1, 1), 3.0e38f, MAX_MAP_GENERATION_LIMIT);
	check(*ceiling->clone(), v2s16(-1, 1), 3.0e38f, MAX_MAP_GENERATION_LIMIT);
	params.np_valley_depth = constant_noise(0.0f);
	params.np_inter_valley_slope = constant_noise(0.0f);
	params.np_terrain_height = constant_noise(std::numeric_limits<float>::infinity());
	auto invalid = createValleysBiomeTerrainSampler(params);
	UASSERT(!invalid->sampleClimate(v2s16(0), unavailable));
	UASSERTEQ(float, unavailable.river_bank_height, 8.0f);
	UASSERTEQ(s16, unavailable.column_max_y, 9);

	// Mountain bodies no longer raise the climate height; the form's mask
	// records that a body can rise in this column.
	params.np_terrain_height = constant_noise(0.0f);
	params.spflags = MGVALLEYS_MOUNTAINS;
	params.np_mountain = constant_noise(1.0f);
	params.np_mountain_height = constant_noise(20.0f);
	for (float cap : {0.0f, 10.0f}) {
		params.mountain_cap = cap;
		auto mountains = createValleysBiomeTerrainSampler(params);
		check(*mountains, v2s16(-17, 31), 0.0f, 0);
		BiomeClimateContext context;
		UASSERT(mountains->sampleClimate(v2s16(-17, 31), context));
		UASSERTEQ(float, context.form.mountain, 20.0f);
		UASSERTEQ(float, context.form.valley_depth, 0.0f);
		UASSERTEQ(float, context.form.base, 0.0f);
	}
	params.spflags = 0;
	auto no_mountains = createValleysBiomeTerrainSampler(params);
	BiomeClimateContext flat_context;
	UASSERT(no_mountains->sampleClimate(v2s16(-17, 31), flat_context));
	UASSERTEQ(float, flat_context.form.mountain, 0.0f);

	MapgenValleysParams varied_params;
	varied_params.seed = 54321;
	varied_params.spflags = MGVALLEYS_MOUNTAINS;
	auto varied = createValleysBiomeTerrainSampler(varied_params);
	auto clone = varied->clone();
	std::vector<BiomeClimateContext> expected;
	for (auto pos : positions) {
		BiomeClimateContext context;
		UASSERT(varied->sampleClimate(pos, context));
		UASSERT(std::isfinite(context.river_bank_height));
		UASSERT(context.column_max_y >= -MAX_MAP_GENERATION_LIMIT &&
			context.column_max_y <= MAX_MAP_GENERATION_LIMIT);
		expected.push_back(context);
	}
	UASSERT(expected.front().river_bank_height != expected.back().river_bank_height);
	varied_params.chunksize = v3s16(9);
	varied_params.mapgen_limit = 32;
	varied_params.floor_y = 1000;
	varied_params.spflags |= MGVALLEYS_CARVE_CLIFFS | MGVALLEYS_REMOVE_FLOATERS;
	auto later_operations = createValleysBiomeTerrainSampler(varied_params);
	for (size_t i = expected.size(); i-- > 0;) {
		for (const BiomeTerrainSampler *sampler :
				{varied.get(), clone.get(), later_operations.get()})
			check(*sampler, positions[i], expected[i].river_bank_height,
				expected[i].column_max_y);
	}

	// Exercise eviction cheaply with varying 2D height and no 3D search.
	params.spflags = 0;
	params.np_terrain_height = {20, 5, v3f(32), 901, 1, 0.5f, 2};
	auto evicted = createValleysBiomeTerrainSampler(params);
	expected.clear();
	for (auto pos : positions) {
		BiomeClimateContext context;
		UASSERT(evicted->sampleClimate(pos, context));
		expected.push_back(context);
	}
	for (s16 z = 200; z < 382; ++z)
	for (s16 x = 200; x < 382; ++x) {
		BiomeClimateContext context;
		UASSERT(evicted->sampleClimate(v2s16(x, z), context));
	}
	for (size_t i = expected.size(); i-- > 0;)
		check(*evicted, positions[i], expected[i].river_bank_height,
			expected[i].column_max_y);
}

void TestMapgen::testEffectiveBiomeSelection()
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	// Effective climate of the fixture column: bank 16, 2D surface 32, all
	// three corrections with altitude_chill 100.
	const ValleysClimate effective = calcValleysClimate(50.0f, 50.0f, 16.0f, 32, 0,
		100.0f, MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS | MGVALLEYS_ALT_DRY);
	// The fixture column has no mountain body and sits on its ridge.
	auto blocked = addTerrainTestBiome(manager, "peak", effective.heat, effective.humidity);
	blocked->mountain_min = 1.0f;
	auto lower = addTerrainTestBiome(manager, "lower_cold", effective.heat, effective.humidity);
	lower->max_pos.Y = 0;
	lower->vertical_blend = 8;
	lower->valley_pos_min = 1.0f;
	auto upper = addTerrainTestBiome(manager, "upper", 50.0f, 50.0f);
	upper->min_pos.Y = 1;
	auto raw_lower = addTerrainTestBiome(manager, "lower_warm", 50.0f, 50.0f);
	raw_lower->max_pos.Y = 0;
	for (Biome *biome : {blocked, lower, upper, raw_lower}) {
		biome->min_pos.X = biome->min_pos.Z = -10;
		biome->max_pos.X = biome->max_pos.Z = 10;
		biome->min_pos.Y = std::max<s16>(biome->min_pos.Y, -100);
		biome->max_pos.Y = std::min<s16>(biome->max_pos.Y, 100);
	}
	// Resolve definitions before cloning into an independent worker registry.
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	auto ndef = const_cast<NodeDefManager *>(server.getNodeDefManager());
	for (Biome *biome : {blocked, lower, upper, raw_lower}) {
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		ndef->pendNodeResolve(biome);
	}
	ndef->setNodeRegistrationStatus(true);
	ndef->runNodeResolveCallbacks();
	std::unique_ptr<BiomeManager> copied_manager(manager.clone());

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	MapgenValleysParams params;
	params.seed = 12345;
	params.spflags = MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS | MGVALLEYS_ALT_DRY;
	params.water_level = 0;
	params.altitude_chill = 100;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	BiomeGenOriginal generator(&manager, &climate, v3s16(16));
	EffectiveBiomeData result;
	UASSERT(!generator.getEffectiveBiomeData(v3s16(0), result));
	generator.setValleysClimate(params);
	std::unique_ptr<BiomeGen> worker_base(generator.clone(copied_manager.get()));
	auto worker = static_cast<BiomeGenOriginal *>(worker_base.get());
	// A point query must not read the last generation pass's mutable maps.
	generator.calcBiomeNoise(v3s16(-8));
	std::fill(generator.heatmap, generator.heatmap + 256, -900.0f);
	std::fill(generator.humidmap, generator.humidmap + 256, 900.0f);
	unsigned int lower_blends = 0;
	unsigned int upper_blends = 0;
	for (s16 y : {s16(-101), s16(-100), s16(-1), s16(0), s16(1), s16(2),
			s16(3), s16(4), s16(5), s16(6), s16(7), s16(8), s16(9), s16(100), s16(101)}) {
		const v3s16 pos(-1, y, 0);
		UASSERT(generator.getEffectiveBiomeData(pos, result));
		UASSERTEQ(float, result.heat, effective.heat);
		UASSERTEQ(float, result.humidity, effective.humidity);
		UASSERTEQ(float, result.raw_heat, 50.0f);
		UASSERTEQ(float, result.raw_humidity, 50.0f);
		UASSERTEQ(float, result.climate_reference_height, 32.0f);
		UASSERTEQ(float, result.river_bank_height, 16.0f);
		UASSERTEQ(float, result.form.base, 16.0f);
		UASSERTEQ(float, result.form.valley_depth, 16.0f);
		UASSERTEQ(float, result.form.valley_pos, 1.0f);
		UASSERTEQ(float, result.form.mountain, 0.0f);
		UASSERTEQ(biome_t, result.biome,
			generator.calcBiomeFromNoise(effective.heat, effective.humidity, pos)->index);
		UASSERTEQ(float, generator.calcHeatAtPoint(pos), 50.0f);
		UASSERTEQ(float, generator.calcHumidityAtPoint(pos), 50.0f);
		if (y < -100 || y > 100) {
			UASSERTEQ(biome_t, result.biome, BIOME_NONE);
		} else if (y <= 0) {
			UASSERTEQ(biome_t, result.biome, lower->index);
			UASSERTEQ(biome_t, generator.calcBiomeAtPoint(pos)->index, raw_lower->index);
		} else if (y <= 8) {
			UASSERT(result.biome == lower->index || result.biome == upper->index);
			lower_blends += result.biome == lower->index;
			upper_blends += result.biome == upper->index;
		} else {
			UASSERTEQ(biome_t, result.biome, upper->index);
		}
		EffectiveBiomeData copied;
		UASSERT(worker->getEffectiveBiomeData(pos, copied));
		UASSERTEQ(biome_t, copied.biome, result.biome);
		UASSERTEQ(float, copied.heat, result.heat);
		UASSERTEQ(float, copied.humidity, result.humidity);
		UASSERTEQ(float, copied.climate_reference_height, result.climate_reference_height);
	}
	UASSERT(lower_blends > 0 && upper_blends > 0);
	UASSERT(generator.getEffectiveBiomeData(v3s16(11, 0, 0), result));
	UASSERTEQ(biome_t, result.biome, BIOME_NONE);

	params.np_terrain_height.offset = 100.0f;
	generator.setValleysClimate(params);
	UASSERT(generator.getEffectiveBiomeData(v3s16(0), result));
	UASSERTEQ(float, result.climate_reference_height, 132.0f);
	UASSERTEQ(float, result.form.base, 116.0f);
	UASSERT(worker->getEffectiveBiomeData(v3s16(0), result));
	UASSERTEQ(float, result.climate_reference_height, 32.0f);
	UASSERTEQ(float, result.form.base, 16.0f);
}

void TestMapgen::testEffectiveClimateSafety()
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	MapgenValleysParams params;
	params.spflags = 0;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(0.0f);
	params.np_inter_valley_slope = constant_noise(0.0f);
	const float largest = std::numeric_limits<float>::max();
	const struct { float heat, humidity; bool valid; } cases[] = {
		{50, 50, true},
		{1.0e30f, 1.0e30f, false},
		{-1.0e30f, -1.0e30f, false},
		{largest, largest, false}, // Finite inputs, overflowing float sum.
		{-largest, -largest, false},
		{largest, -largest, true}, // Cancellation leaves a valid selector seed.
		// Adjacent floats around the selector's signed 64-bit seed boundary.
		{0x1.1c71c6p63f, 0, true},
		{0x1.1c71c8p63f, 0, false}, // Scaled sum is exactly +2^63.
		{-0x1.1c71c8p63f, 0, true}, // Exactly -2^63 is representable.
		{-0x1.1c71cap63f, 0, false},
		{std::numeric_limits<float>::infinity(), 0, false},
		{std::numeric_limits<float>::quiet_NaN(), 0, false},
	};
	for (const auto &test : cases) {
		BiomeParamsOriginal climate;
		climate.np_heat = constant_noise(test.heat);
		climate.np_humidity = constant_noise(test.humidity);
		climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
		BiomeGenOriginal generator(&manager, &climate, v3s16(16));
		generator.setValleysClimate(params);
		ValleysClimate generated;
		EffectiveBiomeClimate full;
		UASSERTEQ(bool, generator.getEffectiveClimate(v2s16(0), generated), test.valid);
		UASSERTEQ(bool, generator.getEffectiveClimate(v2s16(0), full), test.valid);
		for (s16 y : {s16(-MAX_MAP_GENERATION_LIMIT), s16(0),
				s16(MAX_MAP_GENERATION_LIMIT)}) {
			EffectiveBiomeData query;
			UASSERTEQ(bool, generator.getEffectiveBiomeData(v3s16(0, y, 0), query),
				test.valid);
			if (test.valid) {
				UASSERTEQ(float, query.heat, generated.heat);
				UASSERTEQ(float, query.humidity, generated.humidity);
			}
		}
	}

	// Moderate raw climate can become unsafe through a finite terrain bank.
	params.spflags = MGVALLEYS_ALT_CHILL | MGVALLEYS_ALT_DRY;
	params.altitude_chill = 1;
	params.np_terrain_height = constant_noise(1.0e20f);
	BiomeParamsOriginal climate;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	BiomeGenOriginal generator(&manager, &climate, v3s16(16));
	generator.setValleysClimate(params);
	ValleysClimate generated;
	EffectiveBiomeClimate full;
	EffectiveBiomeData query;
	UASSERT(!generator.getEffectiveClimate(v2s16(0), generated));
	UASSERT(!generator.getEffectiveClimate(v2s16(0), full));
	UASSERT(!generator.getEffectiveBiomeData(v3s16(0), query));
}

void TestMapgen::testEffectiveClimateContextDemand()
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	BiomeParamsOriginal climate;
	constexpr u32 corrections = MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_ALT_DRY;
	for (u32 flags : {0U, u32(MGVALLEYS_VARY_RIVER_DEPTH),
			u32(MGVALLEYS_ALT_CHILL), u32(MGVALLEYS_HUMID_RIVERS),
			u32(MGVALLEYS_ALT_DRY), corrections}) {
		MapgenValleysParams params;
		params.spflags = flags;
		BiomeGenOriginal generator(&manager, &climate, v3s16(16));
		generator.setValleysClimate(params);
		auto sampler = std::make_unique<CountingClimateSampler>(
			createValleysBiomeTerrainSampler(params));
		auto *counted = sampler.get();
		generator.setTerrainSampler(std::move(sampler));
		ValleysClimate generated;
		UASSERT(generator.getEffectiveClimate(v2s16(0), generated));
		const unsigned int generation_calls = flags & corrections ? 1 : 0;
		UASSERTEQ(unsigned int, counted->climate_calls, generation_calls);
		EffectiveBiomeClimate full;
		UASSERT(generator.getEffectiveClimate(v2s16(0), full));
		UASSERTEQ(unsigned int, counted->climate_calls, generation_calls + 1);
		UASSERTEQ(float, full.heat, generated.heat);
		UASSERTEQ(float, full.humidity, generated.humidity);
		UASSERT(std::isfinite(full.climate_reference_height));
		UASSERT(std::isfinite(full.river_bank_height));
		EffectiveBiomeData query;
		UASSERT(generator.getEffectiveBiomeData(v3s16(0), query));
		UASSERTEQ(unsigned int, counted->climate_calls, generation_calls + 2);
		UASSERTEQ(float, query.climate_reference_height, full.climate_reference_height);
	}
}

void TestMapgen::testValleysEffectiveClimate(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	NodeDefManager ndef;
	auto add_node = [&](const char *name, content_t source) {
		ContentFeatures def = gamedef->ndef()->get(source);
		def.name = name;
		return ndef.set(name, def);
	};
	const content_t stone = add_node("mapgen_stone", t_CONTENT_STONE);
	const content_t water = add_node("mapgen_water_source", t_CONTENT_WATER);
	const content_t river = add_node("mapgen_river_water_source", t_CONTENT_WATER);
	struct Materials { content_t top, filler, stone; };
	const Materials warm = {
		add_node("test:warm_top", t_CONTENT_GRASS),
		add_node("test:warm_filler", t_CONTENT_BRICK),
		add_node("test:warm_stone", t_CONTENT_STONE),
	};
	const Materials cold = {
		add_node("test:cold_top", t_CONTENT_GRASS),
		add_node("test:cold_filler", t_CONTENT_BRICK),
		add_node("test:cold_stone", t_CONTENT_STONE),
	};
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(&ndef);
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	// The default initially waits on the mock server's registry. Resolve all
	// fixture biomes through the local registry before cloning emerge data.
	server.ndef()->cancelNodeResolveCallback(default_biome);
	ndef.pendNodeResolve(default_biome);
	auto add_biome = [&](const char *name, float heat, const Materials &nodes) {
		auto biome = addTerrainTestBiome(manager, name, heat, 40.0f);
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		biome->m_nodenames[0] = ndef.get(nodes.top).name;
		biome->m_nodenames[1] = ndef.get(nodes.filler).name;
		biome->m_nodenames[2] = ndef.get(nodes.stone).name;
		ndef.pendNodeResolve(biome);
		biome->c_top = nodes.top;
		biome->depth_top = 1;
		biome->c_filler = nodes.filler;
		biome->depth_filler = 2;
		biome->c_stone = nodes.stone;
		biome->c_water = biome->c_water_top = water;
		biome->c_river_water = river;
		biome->c_riverbed = stone;
		return biome->index;
	};
	// The fixture column has bank 16 and a 2D surface at 32; chill with
	// altitude_chill 100 moves heat 50 to 48.6, and heat alone separates the
	// two biomes.
	const float chilled_heat = calcValleysClimate(50.0f, 50.0f, 16.0f, 32, 0,
		100.0f, MGVALLEYS_ALT_CHILL).heat;
	const biome_t warm_id = add_biome("warm", 50.0f, warm);
	const biome_t cold_id = add_biome("cold", chilled_heat, cold);
	ndef.setNodeRegistrationStatus(true);
	ndef.runNodeResolveCallbacks();

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	MapgenValleysParams params;
	params.seed = 12345;
	params.chunksize = v3s16(1);
	params.flags = MG_BIOMES;
	params.water_level = 0;
	params.altitude_chill = 100;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	params.np_filler_depth = constant_noise(0.0f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	BiomeGenOriginal source(&manager, &climate, v3s16(MAP_BLOCKSIZE));
	MetricsBackend metrics;
	EmergeManager emerge(&server, &metrics);
	emerge.ndef = &ndef;

	// The bank is at 16, the 2D surface at 32 and the density surface at 52.
	// Every generation pass uses the 2D surface as the climate height,
	// including empty upper chunks and existing stone; the density relief
	// only shapes the materials. Point queries use the same climate.
	UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(v2s16(0)),
		52.0f);
	constexpr u32 all_climate = MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_ALT_DRY;
	const struct {
		const char *name;
		s16 block_y;
		u32 spflags;
		bool existing_stone;
	} cases[] = {
		{"raw", 3, 0, false},
		{"chill", 3, MGVALLEYS_ALT_CHILL, false},
		{"river humidity", 3, MGVALLEYS_HUMID_RIVERS, false},
		{"altitude dryness", 3, MGVALLEYS_ALT_DRY, false},
		{"below surface", 0, all_climate, false},
		{"clipped surface", 2, all_climate, false},
		{"surface", 3, all_climate, false},
		{"above surface", 4, all_climate, false},
		{"existing surface", 3, all_climate, true},
	};
	for (const auto &test : cases) {
		infostream << "Valleys climate fixture: " << test.name << std::endl;
		params.spflags = test.spflags;
		source.setValleysClimate(params);
		const ValleysClimate expected = calcValleysClimate(50.0f, 50.0f, 16.0f, 32,
			0, 100.0f, test.spflags & all_climate);
		const float expected_heat = expected.heat;
		const float expected_humidity = expected.humidity;
		const biome_t expected_biome = test.spflags & MGVALLEYS_ALT_CHILL ?
			cold_id : warm_id;
		const v3s16 node_min(-16, test.block_y * MAP_BLOCKSIZE, 16);
		const v3s16 node_max = node_min + v3s16(MAP_BLOCKSIZE - 1);
		EffectiveBiomeData before;
		UASSERT(source.getEffectiveBiomeData(node_min, before));
		UASSERTEQ(float, before.climate_reference_height, 32.0f);
		UASSERTEQ(float, before.river_bank_height, 16.0f);
		UASSERTEQ(float, before.heat, expected_heat);
		UASSERTEQ(float, before.humidity, expected_humidity);
		MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
			emerge.getOreManager(), emerge.getDecorationManager(),
			emerge.getSchematicManager()));
		auto generated = static_cast<BiomeGenOriginal *>(mapgen.biomegen);
		auto sampler = std::make_unique<CountingClimateSampler>(
			createValleysBiomeTerrainSampler(params));
		auto *counted = sampler.get();
		generated->setTerrainSampler(std::move(sampler));
		BlockMakeData data;
		data.blockpos_min = data.blockpos_max = v3s16(-1, test.block_y, 1);
		data.seed = params.seed;
		data.nodedef = &ndef;
		data.vmanip = new MapgenTestVManip(VoxelArea(
			node_min - v3s16(MAP_BLOCKSIZE), node_max + v3s16(MAP_BLOCKSIZE)));
		if (test.existing_stone) {
			for (s16 z = node_min.Z; z <= node_max.Z; ++z)
			for (s16 x = node_min.X; x <= node_max.X; ++x)
			for (s16 y = node_min.Y - 1; y <= 52; ++y)
				data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)] = MapNode(stone);
		}
		mapgen.makeChunk(&data);
		UASSERTEQ(unsigned int, counted->climate_calls,
			test.spflags & all_climate ? MAP_BLOCKSIZE * MAP_BLOCKSIZE : 0);
		const auto &materials = expected_biome == cold_id ? cold : warm;
		for (s16 z = node_min.Z; z <= node_max.Z; ++z)
		for (s16 x = node_min.X; x <= node_max.X; ++x) {
			const size_t index = (z - node_min.Z) * MAP_BLOCKSIZE + x - node_min.X;
			UASSERTEQ(float, generated->heatmap[index], expected_heat);
			UASSERTEQ(float, generated->humidmap[index], expected_humidity);
			UASSERTEQ(biome_t, mapgen.biomemap[index],
				node_min.Y > 52 ? BIOME_NONE : expected_biome);
			for (s16 y : {s16(-100), std::min<s16>(52, node_max.Y), s16(100)}) {
				const v3s16 pos(x, y, z);
				EffectiveBiomeData query, parent;
				UASSERT(generated->getEffectiveBiomeData(pos, query));
				UASSERT(source.getEffectiveBiomeData(pos, parent));
				UASSERTEQ(float, query.heat, expected_heat);
				UASSERTEQ(float, query.humidity, expected_humidity);
				UASSERTEQ(float, query.raw_heat, 50.0f);
				UASSERTEQ(float, query.raw_humidity, 50.0f);
				UASSERTEQ(float, parent.heat, query.heat);
				UASSERTEQ(float, parent.humidity, query.humidity);
				UASSERTEQ(biome_t, query.biome, expected_biome);
				UASSERTEQ(biome_t, query.biome, generated->getBiomeAtIndex(index, pos)->index);
				UASSERTEQ(biome_t, source.calcBiomeAtPoint(pos)->index, warm_id);
			}
			for (s16 y = node_min.Y; y <= node_max.Y; ++y) {
				content_t expected = CONTENT_AIR;
				if (y <= 52)
					expected = y == 52 ? materials.top :
						(y >= 50 ? materials.filler : materials.stone);
				const MapNode &node = data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)];
				UASSERTEQ(content_t, node.getContent(), expected);
				UASSERTEQ(u8, node.param1, 0);
				UASSERTEQ(u8, node.param2, 0);
			}
		}
	}

	// The channel has a bank at 16, stone through 10 and unmodified river
	// water through 15. Dry climate changes the water surface before the
	// final climate correction, including when that correction is disabled.
	params.flags = 0;
	params.np_rivers = constant_noise(0.0f);
	constexpr u32 chill_river = MGVALLEYS_ALT_CHILL | MGVALLEYS_VARY_RIVER_DEPTH;
	const struct {
		const char *name;
		u16 altitude_chill;
		u32 spflags;
		float raw_heat;
		float heat;
		float humidity;
		s16 water_top;
	} river_cases[] = {
		{"zero divisor river", 0, chill_river, 400, 85, 40, 13},
		{"minimum divisor river", 1, chill_river, 400, 85, 40, 13},
		{"river depth without corrections", 0, MGVALLEYS_VARY_RIVER_DEPTH, 100, 100, 40, 12},
		{"all flags disabled", 0, 0, 100, 100, 40, 15},
		{"river humidity only", 0, MGVALLEYS_VARY_RIVER_DEPTH | MGVALLEYS_HUMID_RIVERS,
			100, 100, 48, 12},
	};
	for (const auto &test : river_cases) {
		infostream << "Valleys river climate fixture: " << test.name << std::endl;
		params.altitude_chill = test.altitude_chill;
		params.spflags = test.spflags;
		BiomeParamsOriginal river_climate = climate;
		river_climate.np_heat = constant_noise(test.raw_heat);
		river_climate.np_humidity = constant_noise(40.0f);
		BiomeGenOriginal river_source(&manager, &river_climate, v3s16(MAP_BLOCKSIZE));
		MapgenValleys mapgen(&params, new EmergeParams(&emerge, &river_source, &manager,
			emerge.getOreManager(), emerge.getDecorationManager(),
			emerge.getSchematicManager()));
		auto generated = static_cast<BiomeGenOriginal *>(mapgen.biomegen);
		auto sampler = std::make_unique<CountingClimateSampler>(
			createValleysBiomeTerrainSampler(params));
		auto *counted = sampler.get();
		generated->setTerrainSampler(std::move(sampler));
		UASSERTEQ(u16, params.altitude_chill, test.altitude_chill);
		BlockMakeData data;
		data.blockpos_min = data.blockpos_max = v3s16(0);
		data.seed = params.seed;
		data.nodedef = &ndef;
		data.vmanip = new MapgenTestVManip(VoxelArea(v3s16(-MAP_BLOCKSIZE),
			v3s16(2 * MAP_BLOCKSIZE - 1)));
		mapgen.makeChunk(&data);
		UASSERTEQ(unsigned int, counted->climate_calls,
			test.spflags & all_climate ? MAP_BLOCKSIZE * MAP_BLOCKSIZE : 0);
		for (s16 z = 0; z < MAP_BLOCKSIZE; ++z)
		for (s16 x = 0; x < MAP_BLOCKSIZE; ++x) {
			const size_t index = z * MAP_BLOCKSIZE + x;
			UASSERT(std::isfinite(generated->heatmap[index]));
			UASSERT(std::isfinite(generated->humidmap[index]));
			UASSERTEQ(float, generated->heatmap[index], test.heat);
			UASSERTEQ(float, generated->humidmap[index], test.humidity);
			UASSERTEQ(biome_t, mapgen.biomemap[index], BIOME_NONE);
			for (s16 y = 0; y < MAP_BLOCKSIZE; ++y) {
				const content_t expected = y <= 10 ? stone :
					(y <= test.water_top ? river : CONTENT_AIR);
				const MapNode &node = data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)];
				UASSERTEQ(content_t, node.getContent(), expected);
				UASSERTEQ(u8, node.param1, 0);
				UASSERTEQ(u8, node.param2, 0);
			}
		}
	}
}

void TestMapgen::testValleysFormGeneration(IGameDef *gamedef)
{
	// Three biomes on one climate point, told apart only by the column form,
	// through the real generation path: terrain pass, sampler cache, bulk
	// biome selection and the materials it places. Every column of a fixture
	// chunk has the same form, so the whole biomemap must agree.
	MockServer server(getTestTempDirectory());
	NodeDefManager ndef;
	auto add_node = [&](const char *name, content_t source) {
		ContentFeatures def = gamedef->ndef()->get(source);
		def.name = name;
		return ndef.set(name, def);
	};
	const content_t stone = add_node("mapgen_stone", t_CONTENT_STONE);
	const content_t water = add_node("mapgen_water_source", t_CONTENT_WATER);
	const content_t river = add_node("mapgen_river_water_source", t_CONTENT_WATER);
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(&ndef);
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	server.ndef()->cancelNodeResolveCallback(default_biome);
	ndef.pendNodeResolve(default_biome);
	auto add_biome = [&](const char *name) {
		std::string top_name = std::string("test:") + name + "_top";
		const content_t top = add_node(top_name.c_str(), t_CONTENT_GRASS);
		auto biome = addTerrainTestBiome(manager, name, 50.0f, 50.0f);
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		biome->m_nodenames[0] = top_name;
		ndef.pendNodeResolve(biome);
		biome->c_top = top;
		biome->depth_top = 1;
		biome->c_filler = biome->c_stone = stone;
		biome->depth_filler = 0;
		biome->c_water = biome->c_water_top = water;
		biome->c_river_water = river;
		biome->c_riverbed = stone;
		return biome;
	};
	// Registration order breaks ties: the peak must precede the ridge.
	auto peak = add_biome("peak");
	peak->mountain_min = 10.0f;
	auto ridge = add_biome("ridge");
	ridge->valley_pos_min = 0.5f;
	ridge->mountain_max = 5.0f;
	auto floor = add_biome("floor");
	floor->valley_pos_max = 0.5f;
	ndef.setNodeRegistrationStatus(true);
	ndef.runNodeResolveCallbacks();

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	MapgenValleysParams params;
	params.seed = 12345;
	params.chunksize = v3s16(1);
	params.flags = MG_BIOMES;
	params.water_level = 0;
	params.altitude_chill = 100;
	params.mountain_cap = 0.0f;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	params.np_filler_depth = constant_noise(0.0f);
	params.np_mountain = constant_noise(1.0f);
	params.np_mountain_height = constant_noise(20.0f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	MetricsBackend metrics;
	EmergeManager emerge(&server, &metrics);
	emerge.ndef = &ndef;

	constexpr u32 all_climate = MGVALLEYS_ALT_CHILL | MGVALLEYS_HUMID_RIVERS |
		MGVALLEYS_ALT_DRY;
	const struct {
		const char *name;
		float rivers;      // constant river noise: 100 puts the column on the ridge
		u32 spflags;
		s16 block_y;       // the block holding the density surface
		Biome *expected;
	} cases[] = {
		{"ridge", 100.0f, 0, 3, ridge},
		{"ridge with corrections", 100.0f, all_climate, 3, ridge},
		{"valley floor", 0.06f, 0, 1, floor},
		{"valley floor with corrections", 0.06f, all_climate, 1, floor},
		{"mountain mask", 100.0f, MGVALLEYS_MOUNTAINS, 3, peak},
		{"mountain mask with corrections", 100.0f, MGVALLEYS_MOUNTAINS | all_climate, 3, peak},
	};
	for (const auto &test : cases) {
		infostream << "Valleys form fixture: " << test.name << std::endl;
		params.spflags = test.spflags;
		params.np_rivers = constant_noise(test.rivers);
		BiomeGenOriginal source(&manager, &climate, v3s16(MAP_BLOCKSIZE));
		source.setValleysClimate(params);
		MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
			emerge.getOreManager(), emerge.getDecorationManager(),
			emerge.getSchematicManager()));
		auto generated = static_cast<BiomeGenOriginal *>(mapgen.biomegen);
		const v3s16 node_min(-16, test.block_y * MAP_BLOCKSIZE, 16);
		const v3s16 node_max = node_min + v3s16(MAP_BLOCKSIZE - 1);
		BlockMakeData data;
		data.blockpos_min = data.blockpos_max = v3s16(-1, test.block_y, 1);
		data.seed = params.seed;
		data.nodedef = &ndef;
		data.vmanip = new MapgenTestVManip(VoxelArea(
			node_min - v3s16(MAP_BLOCKSIZE), node_max + v3s16(MAP_BLOCKSIZE)));
		mapgen.makeChunk(&data);
		for (s16 z = node_min.Z; z <= node_max.Z; ++z)
		for (s16 x = node_min.X; x <= node_max.X; ++x) {
			const size_t index = (z - node_min.Z) * MAP_BLOCKSIZE + x - node_min.X;
			UASSERTEQ(biome_t, mapgen.biomemap[index], test.expected->index);
			// The top solid node of the column wears the expected biome.
			s16 top = node_max.Y;
			while (top >= node_min.Y && data.vmanip->m_data[
					data.vmanip->m_area.index(x, top, z)].getContent() == CONTENT_AIR)
				--top;
			UASSERT(top >= node_min.Y);
			UASSERTEQ(content_t, data.vmanip->m_data[
				data.vmanip->m_area.index(x, top, z)].getContent(), test.expected->c_top);
			EffectiveBiomeData query;
			UASSERT(generated->getEffectiveBiomeData(v3s16(x, top, z), query));
			UASSERTEQ(biome_t, query.biome, test.expected->index);
			UASSERT(source.getEffectiveBiomeData(v3s16(x, top, z), query));
			UASSERTEQ(biome_t, query.biome, test.expected->index);
		}
	}
}

void TestMapgen::testBiomeFormRanges()
{
	Biome biome;
	UASSERT(!biome.hasClimateBounds());
	UASSERT(!biome.hasFormConstraints());
	UASSERT(biome.matchesClimate(-1.0e30f, 1.0e30f));
	UASSERT(biome.matchesForm({-1000.0f, 0.0f, 0.0f, 0.0f}));
	UASSERT(biome.matchesForm({1000.0f, 1000.0f, 1.0f, 1000.0f}));

	biome.heat_min = 14.0f;
	biome.heat_max = 28.0f;
	UASSERT(biome.hasClimateBounds() && !biome.hasFormConstraints());
	UASSERT(biome.matchesClimate(14.0f, -50.0f));
	UASSERT(biome.matchesClimate(28.0f, 200.0f));
	UASSERT(!biome.matchesClimate(13.99f, 50.0f));
	UASSERT(!biome.matchesClimate(28.01f, 50.0f));
	biome.humidity_min = 19.0f;
	UASSERT(biome.matchesClimate(20.0f, 19.0f));
	UASSERT(!biome.matchesClimate(20.0f, 18.99f));

	// Exercise each form bound by itself, then a clone with all of them.
	Biome form;
	form.valley_pos_max = 0.05f;
	UASSERT(form.hasFormConstraints() && !form.hasClimateBounds());
	UASSERT(form.matchesForm({0.0f, 0.0f, 0.05f, 0.0f}));
	UASSERT(!form.matchesForm({0.0f, 0.0f, 0.0501f, 0.0f}));
	form.valley_pos_max = 1.0f;
	UASSERT(!form.hasFormConstraints());
	form.valley_pos_min = 0.15f;
	UASSERT(form.hasFormConstraints());
	UASSERT(form.matchesForm({0.0f, 0.0f, 0.15f, 0.0f}));
	UASSERT(!form.matchesForm({0.0f, 0.0f, 0.1499f, 0.0f}));
	form.valley_pos_min = 0.0f;
	form.base_min = 10.0f;
	form.valley_depth_max = 9.0f;
	UASSERT(form.matchesForm({10.0f, 9.0f, 0.5f, 0.0f}));
	UASSERT(!form.matchesForm({9.99f, 9.0f, 0.5f, 0.0f}));
	UASSERT(!form.matchesForm({10.0f, 9.01f, 0.5f, 0.0f}));
	form.base_min = -std::numeric_limits<float>::infinity();
	form.valley_depth_max = std::numeric_limits<float>::infinity();
	form.valley_depth_min = 9.0f;
	UASSERT(form.hasFormConstraints());
	UASSERT(!form.matchesForm({0.0f, 8.99f, 0.0f, 0.0f}));
	form.valley_depth_min = 0.0f;
	form.mountain_min = 40.0f;
	UASSERT(form.hasFormConstraints());
	UASSERT(form.matchesForm({0.0f, 0.0f, 0.0f, 40.0f}));
	UASSERT(!form.matchesForm({0.0f, 0.0f, 0.0f, 39.99f}));
	form.mountain_min = 0.0f;
	form.mountain_max = 10.0f;
	UASSERT(!form.matchesForm({0.0f, 0.0f, 0.0f, 10.01f}));
	form.mountain_max = std::numeric_limits<float>::infinity();
	form.base_max = -5.0f;
	UASSERT(form.hasFormConstraints());
	UASSERT(form.matchesForm({-5.0f, 0.0f, 0.0f, 0.0f}));
	UASSERT(!form.matchesForm({-4.99f, 0.0f, 0.0f, 0.0f}));

	form.heat_max = 63.0f;
	UASSERT(form.hasClimateBounds() && form.hasFormConstraints());
	UASSERT(form.matchesClimate(63.0f, 0.0f) && !form.matchesClimate(63.01f, 0.0f));
}

void TestMapgen::testBiomeFormParsing()
{
	std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
	UASSERT(state);
	lua_State *L = state.get();
	std::unique_ptr<NodeDefManager> ndef(createNodeDefManager());
	auto number = [L](const char *name, lua_Number value) {
		lua_pushnumber(L, value);
		lua_setfield(L, -2, name);
	};
	auto reject = [&]() {
		bool rejected = false;
		try {
			std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		} catch (const LuaError &) {
			rejected = true;
		}
		lua_settop(L, 0);
		UASSERT(rejected);
	};

	lua_newtable(L);
	{
		std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		UASSERT(parsed && !parsed->hasClimateBounds() && !parsed->hasFormConstraints());
		UASSERT(std::isinf(parsed->heat_min) && parsed->heat_min < 0.0f);
		UASSERT(std::isinf(parsed->mountain_max) && parsed->mountain_max > 0.0f);
		UASSERTEQ(float, parsed->valley_pos_max, 1.0f);
		UASSERTEQ(int, parsed->priority, 0);
	}
	lua_settop(L, 0);
	lua_newtable(L);
	number("priority", 2.0);
	number("heat_min", -14.0);
	number("heat_max", 28.0);
	number("humidity_max", 51.0);
	number("base_min", -3.0);
	number("base_max", 10.0);
	number("valley_depth_min", 3.0);
	number("valley_depth_max", 9.0);
	number("valley_pos_min", 0.05);
	number("valley_pos_max", 0.15);
	number("mountain_min", 10.0);
	number("mountain_max", 40.0);
	{
		std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		UASSERT(parsed && parsed->hasClimateBounds() && parsed->hasFormConstraints());
		UASSERTEQ(float, parsed->heat_min, -14.0f);
		UASSERTEQ(float, parsed->heat_max, 28.0f);
		UASSERT(std::isinf(parsed->humidity_min) && parsed->humidity_min < 0.0f);
		UASSERTEQ(float, parsed->humidity_max, 51.0f);
		UASSERTEQ(float, parsed->base_min, -3.0f);
		UASSERTEQ(float, parsed->base_max, 10.0f);
		UASSERTEQ(float, parsed->valley_depth_min, 3.0f);
		UASSERTEQ(float, parsed->valley_depth_max, 9.0f);
		UASSERTEQ(float, parsed->valley_pos_min, 0.05f);
		UASSERTEQ(float, parsed->valley_pos_max, 0.15f);
		UASSERTEQ(float, parsed->mountain_min, 10.0f);
		UASSERTEQ(float, parsed->mountain_max, 40.0f);
		UASSERTEQ(int, parsed->priority, 2);
	}
	lua_settop(L, 0);

	// A priority is a whole number in the s16 range; anything else is rejected.
	for (lua_Number invalid : {1.5, 40000.0, -40000.0,
			std::numeric_limits<lua_Number>::infinity(),
			std::numeric_limits<lua_Number>::quiet_NaN()}) {
		lua_newtable(L);
		number("priority", invalid);
		reject();
	}
	lua_newtable(L);
	lua_pushstring(L, "1");
	lua_setfield(L, -2, "priority");
	reject();
	lua_newtable(L);
	number("priority", -3.0);
	{
		std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		UASSERT(parsed);
		UASSERTEQ(int, parsed->priority, -3);
	}
	lua_settop(L, 0);

	const lua_Number inf = std::numeric_limits<lua_Number>::infinity();
	const lua_Number nan = std::numeric_limits<lua_Number>::quiet_NaN();
	const lua_Number beyond_float = static_cast<lua_Number>(
		std::numeric_limits<float>::max()) * 2.0;
	for (const char *name : {"heat_min", "heat_max", "humidity_min",
			"humidity_max", "base_min", "base_max"}) {
		for (lua_Number invalid : {inf, -inf, nan, 1.0e-300, -1.0e-300,
				beyond_float, -beyond_float}) {
			lua_newtable(L);
			number(name, invalid);
			reject();
		}
		lua_newtable(L);
		lua_pushstring(L, "10");
		lua_setfield(L, -2, name);
		reject();
	}
	for (const char *name : {"valley_depth_min", "valley_depth_max",
			"valley_pos_min", "valley_pos_max", "mountain_min", "mountain_max"}) {
		for (lua_Number invalid : {-1.0, inf, nan, 1.0e-300}) {
			lua_newtable(L);
			number(name, invalid);
			reject();
		}
	}
	for (const char *name : {"valley_pos_min", "valley_pos_max"}) {
		lua_newtable(L);
		number(name, 1.0001);
		reject();
	}
	lua_newtable(L);
	number("heat_min", 30.0);
	number("heat_max", 20.0);
	reject();
	lua_newtable(L);
	number("valley_pos_min", 0.5);
	number("valley_pos_max", 0.25);
	reject();

	// Negative signed bounds, zero and a single unbounded side are accepted.
	lua_newtable(L);
	number("base_max", -5.0);
	number("heat_min", -30.0);
	number("mountain_min", 0.0);
	{
		std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		UASSERT(parsed);
		UASSERTEQ(float, parsed->base_max, -5.0f);
		UASSERT(std::isinf(parsed->base_min) && parsed->base_min < 0.0f);
		UASSERTEQ(float, parsed->heat_min, -30.0f);
		UASSERTEQ(float, parsed->mountain_min, 0.0f);
	}
	lua_settop(L, 0);
}

void TestMapgen::testBiomeFormSelection()
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	// Two biomes share one climate point and differ by valley position; a
	// peak needs a mountain mask; a hot cell needs heat the point lacks; the
	// last biome has no bounds and stands in when nothing else can.
	auto floor = addTerrainTestBiome(manager, "floor", 50.0f, 50.0f);
	floor->valley_pos_max = 0.5f;
	auto ridge = addTerrainTestBiome(manager, "ridge", 50.0f, 50.0f);
	ridge->valley_pos_min = 0.5f;
	ridge->valley_pos_max = 0.8f;
	auto peak = addTerrainTestBiome(manager, "peak", 50.0f, 50.0f);
	peak->mountain_min = 1.0f;
	auto hot = addTerrainTestBiome(manager, "hot", 65.0f, 50.0f);
	hot->heat_min = 60.0f;
	auto anywhere = addTerrainTestBiome(manager, "anywhere", 80.0f, 80.0f);
	// Two biomes on one cold cell split by the variant axis at zero.
	auto even = addTerrainTestBiome(manager, "even", 15.0f, 15.0f);
	even->heat_max = 30.0f;
	even->variant_max = 0.0f;
	auto odd = addTerrainTestBiome(manager, "odd", 15.0f, 15.0f);
	odd->heat_max = 30.0f;
	odd->variant_min = 0.0f;
	// Resolve definitions before cloning into an independent worker registry.
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	auto ndef = const_cast<NodeDefManager *>(server.getNodeDefManager());
	for (Biome *biome : {floor, ridge, peak, hot, anywhere, even, odd}) {
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		ndef->pendNodeResolve(biome);
	}
	ndef->setNodeRegistrationStatus(true);
	ndef->runNodeResolveCallbacks();

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	MapgenValleysParams params;
	params.seed = 12345;
	params.spflags = 0;
	params.water_level = 0;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	BiomeGenOriginal generator(&manager, &climate, v3s16(16));
	const v3s16 pos(-1, 0, 0);

	// Without a column model every form-bound biome is ineligible.
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos)->index,
		anywhere->index);
	generator.setTerrainSampler(std::make_unique<TestTerrainSampler>(0.0f));
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos)->index,
		anywhere->index);

	// The fixture column sits on its ridge with no mountain: the ridge is
	// rejected by its own upper bound, so the peak cannot win either.
	generator.setValleysClimate(params);
	EffectiveBiomeData result;
	UASSERT(generator.getEffectiveBiomeData(pos, result));
	UASSERTEQ(float, result.form.valley_pos, 1.0f);
	UASSERTEQ(float, result.form.mountain, 0.0f);
	UASSERTEQ(biome_t, result.biome, anywhere->index);
	UASSERTEQ(biome_t, generator.calcBiomeAtPoint(pos)->index, anywhere->index);

	// A supplied form replaces sampling; climate bounds apply before distance.
	BiomeTerrainForm supplied{0.0f, 16.0f, 0.25f, 0.0f};
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		floor->index);
	supplied.valley_pos = 0.6f;
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		ridge->index);
	supplied.valley_pos = 0.9f;
	supplied.mountain = 5.0f;
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		peak->index);
	supplied.mountain = 0.0f;
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		anywhere->index);
	supplied.valley_pos = 0.6f;
	// Heat 58 is nearer the hot cell than the ridge, but below the cell's bound.
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(58.0f, 50.0f, pos, &supplied)->index,
		ridge->index);
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(62.0f, 50.0f, pos, &supplied)->index,
		hot->index);

	// The form blend noise moves the region level and the valley depth that
	// selection and queries see, the depth never below zero. The fixture
	// column has base 16: a bound of 20 rejects it until the blend adds 5.
	anywhere->base_min = 20.0f;
	UASSERTEQ(biome_t, generator.calcBiomeAtPoint(pos)->index, BIOME_NONE);
	climate.np_base_blend = constant_noise(5.0f);
	climate.np_valley_depth_blend = constant_noise(-20.0f);
	BiomeTerrainForm blended;
	UASSERT(generator.getBiomeForm(v2s16(pos.X, pos.Z), blended));
	UASSERTEQ(float, blended.base, 21.0f);
	UASSERTEQ(float, blended.valley_depth, 0.0f);
	UASSERTEQ(biome_t, generator.calcBiomeAtPoint(pos)->index, anywhere->index);
	UASSERT(generator.getEffectiveBiomeData(pos, result));
	UASSERTEQ(float, result.form.base, 21.0f);
	UASSERTEQ(float, result.form.valley_depth, 0.0f);
	UASSERTEQ(biome_t, result.biome, anywhere->index);
	climate.np_base_blend = climate.np_valley_depth_blend = constant_noise(0.0f);
	anywhere->base_min = -std::numeric_limits<float>::infinity();

	// The variant axis: the noise decides between two biomes on one climate
	// point, takes no part in the distance, and the query reports it.
	climate.np_variant = constant_noise(-0.5f);
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(15.0f, 15.0f, pos, &supplied)->index,
		even->index);
	climate.np_variant = constant_noise(0.5f);
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(15.0f, 15.0f, pos, &supplied)->index,
		odd->index);
	UASSERT(generator.getEffectiveBiomeData(pos, result));
	UASSERTEQ(float, result.variant, 0.5f);
	// A biome outside its variant bound cannot win however close it is.
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		ridge->index);
	climate.np_variant = constant_noise(0.0f);

	// A clone carries the bounds and its own column model.
	std::unique_ptr<BiomeManager> copied_manager(manager.clone());
	auto copied_ridge = static_cast<Biome *>(copied_manager->getRaw(ridge->index));
	UASSERTEQ(float, copied_ridge->valley_pos_min, 0.5f);
	UASSERTEQ(float, copied_ridge->valley_pos_max, 0.8f);
	auto copied_hot = static_cast<Biome *>(copied_manager->getRaw(hot->index));
	UASSERTEQ(float, copied_hot->heat_min, 60.0f);
	UASSERT(std::isinf(copied_hot->heat_max));
	auto copied_odd = static_cast<Biome *>(copied_manager->getRaw(odd->index));
	UASSERTEQ(float, copied_odd->variant_min, 0.0f);
	UASSERT(std::isinf(copied_odd->variant_max));
	std::unique_ptr<BiomeGen> worker_base(generator.clone(copied_manager.get()));
	auto worker = static_cast<BiomeGenOriginal *>(worker_base.get());
	UASSERT(worker->getEffectiveBiomeData(pos, result));
	UASSERTEQ(biome_t, result.biome, anywhere->index);
	UASSERTEQ(biome_t, worker->calcBiomeFromNoise(50.0f, 50.0f, pos, &supplied)->index,
		ridge->index);
}

void TestMapgen::testBiomePriority()
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	// Four biomes on one climate point: a plain up to 40 with a long blend
	// above it, an upland from 20 to 59 blending two nodes up, a summit
	// from 60, and a hot cell nearer to warm climates. Upland and summit
	// outrank the other two.
	auto plain = addTerrainTestBiome(manager, "plain", 50.0f, 50.0f);
	plain->max_pos.Y = 40;
	plain->vertical_blend = 100;
	auto upland = addTerrainTestBiome(manager, "upland", 50.0f, 50.0f);
	upland->min_pos.Y = 20;
	upland->max_pos.Y = 59;
	upland->vertical_blend = 2;
	upland->priority = 1;
	auto summit = addTerrainTestBiome(manager, "summit", 50.0f, 50.0f);
	summit->min_pos.Y = 60;
	summit->priority = 1;
	auto hot = addTerrainTestBiome(manager, "hot", 65.0f, 50.0f);
	hot->heat_min = 60.0f;
	// Resolve definitions before cloning into an independent worker registry.
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	auto ndef = const_cast<NodeDefManager *>(server.getNodeDefManager());
	for (Biome *biome : {plain, upland, summit, hot}) {
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		ndef->pendNodeResolve(biome);
	}
	ndef->setNodeRegistrationStatus(true);
	ndef->runNodeResolveCallbacks();
	BiomeParamsOriginal climate;
	climate.np_heat = climate.np_humidity =
		NoiseParams(50.0f, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	climate.np_heat_blend = climate.np_humidity_blend =
		NoiseParams(0.0f, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	BiomeGenOriginal generator(&manager, &climate, v3s16(16));
	auto at = [&](float heat, s16 y) {
		return generator.calcBiomeFromNoise(heat, 50.0f, v3s16(0, y, 0))->index;
	};
	auto seen = [&](s16 y_min, s16 y_max, bool &upland_seen, bool &summit_seen) {
		upland_seen = summit_seen = false;
		for (s16 y = y_min; y <= y_max; ++y) {
			for (float heat = 0.0f; heat <= 100.0f; heat += 1.0f) {
				biome_t chosen = at(heat, y);
				upland_seen |= chosen == upland->index;
				summit_seen |= chosen == summit->index;
			}
		}
	};

	// Below the upland the plain and the hot cell compete by distance alone.
	UASSERTEQ(biome_t, at(50.0f, 10), plain->index);
	UASSERTEQ(biome_t, at(62.0f, 10), hot->index);
	// Priority beats distance: the hot cell is nearer, the upland outranks it.
	UASSERTEQ(biome_t, at(62.0f, 30), upland->index);
	// A lower-priority blend candidate never dithers into a higher one: the
	// plain's blend zone reaches 140, yet the upland holds 41..59 everywhere.
	for (s16 y = 41; y <= 59; ++y)
		for (float heat : {30.0f, 50.0f, 62.0f})
			UASSERTEQ(biome_t, at(heat, y), upland->index);
	// Equal priorities blend as before: the upland's two nodes above 59
	// dither against the summit, and from 62 the summit stands alone.
	bool upland_seen, summit_seen;
	seen(60, 61, upland_seen, summit_seen);
	UASSERT(upland_seen && summit_seen);
	for (s16 y = 60; y <= 61; ++y)
		for (float heat = 0.0f; heat <= 100.0f; heat += 1.0f) {
			biome_t chosen = at(heat, y);
			UASSERT(chosen == upland->index || chosen == summit->index);
		}
	UASSERTEQ(biome_t, at(50.0f, 62), summit->index);
	UASSERTEQ(biome_t, at(70.0f, 62), summit->index);

	// A higher-priority blend candidate still dithers into a lower one in range.
	summit->priority = 0;
	seen(60, 61, upland_seen, summit_seen);
	UASSERT(upland_seen && summit_seen);
	summit->priority = 1;

	// Without the priority the nearer hot cell takes the upland's range.
	upland->priority = 0;
	UASSERTEQ(biome_t, at(62.0f, 30), hot->index);
	upland->priority = 1;

	// A clone carries the priority.
	std::unique_ptr<BiomeManager> copied_manager(manager.clone());
	auto copied_upland = static_cast<Biome *>(copied_manager->getRaw(upland->index));
	UASSERTEQ(int, copied_upland->priority, 1);
	auto copied_hot = static_cast<Biome *>(copied_manager->getRaw(hot->index));
	UASSERTEQ(int, copied_hot->priority, 0);
}

void TestMapgen::testBiomeNumericDistances(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	BiomeParamsOriginal params;
	const float tiny_weight = std::numeric_limits<float>::denorm_min();
	const float max_float = std::numeric_limits<float>::max();
	{
		MockBiomeManager manager(&server);
		manager.setNodeDefManager(gamedef->getNodeDefManager());
		auto restricted = addTerrainTestBiome(manager, "restricted", 50.0f, 50.0f);
		restricted->heat_min = 60.0f;
		auto reserve = addTerrainTestBiome(manager, "reserve", 52.0f, 50.0f);
		reserve->weight = tiny_weight;
		BiomeGenOriginal generator(&manager, &params, v3s16(16));

		// A bound filter must leave this registered, unbounded reserve
		// selectable even when the weighted float distance overflows.
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == reserve);
		reserve->heat_point = 1.0e20f;
		reserve->weight = 1.0f;
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == reserve);
		reserve->heat_point = max_float;
		reserve->humidity_point = -max_float;
		reserve->weight = tiny_weight;
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == reserve);

		// Two overflowing distances still have an ordering, independent of
		// registration order. The larger weight can compensate a huge center.
		reserve->heat_point = 52.0f;
		reserve->humidity_point = 50.0f;
		auto nearer = addTerrainTestBiome(manager, "nearer", 51.0f, 50.0f);
		nearer->weight = tiny_weight;
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == nearer);
		reserve->heat_point = 1.0e20f;
		reserve->weight = 1.0f;
		nearer->heat_point = 2.0e20f;
		nearer->weight = 1.0f;
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == reserve);
		reserve->weight = max_float;
		nearer->heat_point = 0.0f; // finite float distance 2500
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == reserve);
		nearer->heat_point = 51.0f; // finite float distance 1
		UASSERT(generator.calcBiomeFromNoise(50, 50, v3s16(0)) == nearer);

		// Filtering remains authoritative even when all distances overflow.
		nearer->heat_point = 2.0e20f;
		reserve->heat_min = nearer->heat_min = 60.0f;
		UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50, 50,
			v3s16(0))->index, BIOME_NONE);
	}
	{
		MockBiomeManager manager(&server);
		manager.setNodeDefManager(gamedef->getNodeDefManager());
		// This float distance is exactly FLT_MAX, which used to equal the
		// initial minimum and leave an otherwise eligible biome unselected.
		auto boundary = addTerrainTestBiome(manager, "boundary", 0x1.fffffep63f, 0x1p52f);
		BiomeGenOriginal generator(&manager, &params, v3s16(16));
		UASSERT(generator.calcBiomeFromNoise(0, 0, v3s16(0)) == boundary);
	}
	{
		MockBiomeManager manager(&server);
		manager.setNodeDefManager(gamedef->getNodeDefManager());
		// Both ordinary distances round to 1 in float, though the second is
		// closer in double. Keep the original first-registration tie break.
		auto first = addTerrainTestBiome(manager, "first", 1.0f, 0x1p-13f);
		addTerrainTestBiome(manager, "second", 1.0f, 0.0f);
		BiomeGenOriginal generator(&manager, &params, v3s16(16));
		UASSERT(generator.calcBiomeFromNoise(0, 0, v3s16(0)) == first);
	}
}

void TestMapgen::testBiomeNumericBlending(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager control_manager(&server), exceptional_manager(&server);
	control_manager.setNodeDefManager(gamedef->getNodeDefManager());
	exceptional_manager.setNodeDefManager(gamedef->getNodeDefManager());
	auto lower = addTerrainTestBiome(control_manager, "lower", 52.0f, 50.0f);
	lower->max_pos.Y = 0;
	lower->vertical_blend = 8;
	auto upper = addTerrainTestBiome(control_manager, "upper", 53.0f, 50.0f);
	upper->min_pos.Y = 1;
	auto wide_lower = addTerrainTestBiome(exceptional_manager, "lower", 52.0f, 50.0f);
	wide_lower->max_pos.Y = 0;
	wide_lower->vertical_blend = 8;
	auto wide_upper = addTerrainTestBiome(exceptional_manager, "upper", 53.0f, 50.0f);
	wide_upper->min_pos.Y = 1;
	BiomeParamsOriginal params;
	BiomeGenOriginal control(&control_manager, &params, v3s16(16));
	BiomeGenOriginal exceptional(&exceptional_manager, &params, v3s16(16));

	for (bool huge_centers : {false, true}) {
		wide_lower->heat_point = huge_centers ? 1.0e20f : 52.0f;
		wide_upper->heat_point = huge_centers ? 2.0e20f : 53.0f;
		wide_lower->weight = wide_upper->weight = huge_centers ? 1.0f :
			std::numeric_limits<float>::denorm_min();
		unsigned int blended = 0, unblended = 0;
		for (s16 y = 0; y <= 9; ++y) {
			auto expected = control.calcBiomeFromNoise(50, 50, v3s16(0, y, 0));
			auto actual = exceptional.calcBiomeFromNoise(50, 50, v3s16(0, y, 0));
			UASSERTEQ(std::string, actual->name, expected->name);
			if (y > 0 && y <= 8) {
				blended += actual == wide_lower;
				unblended += actual == wide_upper;
			}
		}
		UASSERT(blended > 0 && unblended > 0);
	}

	// The blend candidate can overflow internally yet have a smaller
	// weighted distance than an ordinary main-band candidate (about 29 < 400).
	wide_lower->weight = std::numeric_limits<float>::max();
	wide_upper->heat_point = upper->heat_point = 70.0f;
	for (s16 y = 1; y <= 8; ++y)
		UASSERTEQ(std::string,
			exceptional.calcBiomeFromNoise(50, 50, v3s16(0, y, 0))->name,
			control.calcBiomeFromNoise(50, 50, v3s16(0, y, 0))->name);

	// With no eligible main-band biome, the same blend roll must still
	// choose the lower biome or the default, rather than always defaulting.
	upper->min_pos.Y = wide_upper->min_pos.Y = 10;
	for (s16 y = 1; y <= 8; ++y)
		UASSERTEQ(std::string,
			exceptional.calcBiomeFromNoise(50, 50, v3s16(0, y, 0))->name,
			control.calcBiomeFromNoise(50, 50, v3s16(0, y, 0))->name);
}

void TestMapgen::testBiomeFormClone(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	auto biome = addTerrainTestBiome(manager, "bounded", 50.0f, 50.0f);
	biome->valley_pos_min = 0.5f;
	biome->valley_depth_max = 20.0f;
	biome->heat_max = 60.0f;
	biome->priority = 2;
	// Worker cloning happens after node registration. Resolve both the
	// default biome and this fixture through the same server-side registry.
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	biome->m_nodenames = default_biome->m_nodenames;
	biome->m_nnlistsizes = default_biome->m_nnlistsizes;
	auto ndef = const_cast<NodeDefManager *>(server.getNodeDefManager());
	ndef->pendNodeResolve(biome);
	ndef->setNodeRegistrationStatus(true);
	ndef->runNodeResolveCallbacks();
	UASSERT(default_biome->isResolveDone() && biome->isResolveDone());
	std::unique_ptr<BiomeManager> copied_manager(manager.clone());
	auto copied_biome = static_cast<Biome *>(copied_manager->getRaw(biome->index));
	UASSERT(copied_biome != biome);
	UASSERTEQ(float, copied_biome->valley_pos_min, 0.5f);
	UASSERTEQ(float, copied_biome->valley_depth_max, 20.0f);
	UASSERTEQ(float, copied_biome->heat_max, 60.0f);
	UASSERTEQ(int, copied_biome->priority, 2);

	BiomeParamsOriginal params;
	params.seed = 54321;
	BiomeGenOriginal original(&manager, &params, v3s16(16));
	auto provider = std::make_unique<TestTerrainSampler>(100.0f,
		BiomeTerrainForm{10.0f, 4.0f, 0.75f, 0.0f});
	auto *probe = provider.get();
	original.setTerrainSampler(std::move(provider));
	std::unique_ptr<BiomeGen> clone_base(original.clone(copied_manager.get()));
	auto *cloned = static_cast<BiomeGenOriginal *>(clone_base.get());
	UASSERT(original.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == biome);
	UASSERT(cloned->calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == copied_biome);

	// Reconfiguring one sampler must not affect the cloned worker's sampler.
	probe->form.valley_pos = 0.25f;
	UASSERTEQ(biome_t, original.calcBiomeFromNoise(50.0f, 50.0f,
			v3s16(0))->index, BIOME_NONE);
	UASSERT(cloned->calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == copied_biome);
	float height = 0.0f;
	UASSERT(cloned->getBiomeTerrainHeight(v2s16(-31, 15), height));
	UASSERTEQ(float, height, 100.0f);
	probe->height = std::numeric_limits<float>::quiet_NaN();
	UASSERT(!original.getBiomeTerrainHeight(v2s16(0), height));
	UASSERT(cloned->getBiomeTerrainHeight(v2s16(0), height));
}

void TestMapgen::testBiomeTerrainValleys()
{
	MapgenValleysParams params;
	params.seed = 12345;
	params.spflags = 0;
	params.water_level = 0;
	params.np_terrain_height.offset = 20.0f;
	params.np_terrain_height.scale = 0.0f;
	params.np_valley_depth.offset = params.np_valley_depth.scale = 0.0f;
	params.np_inter_valley_slope.offset = params.np_inter_valley_slope.scale = 0.0f;
	params.np_inter_valley_fill.offset = params.np_inter_valley_fill.scale = 0.0f;
	params.np_rivers.offset = 1.0f;
	params.np_rivers.scale = 0.0f;
	params.np_valley_profile.offset = 1.0f;
	params.np_valley_profile.scale = 0.0f;
	auto flat = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, flat->sampleHeight(v2s16(-17, 31)), 19.0f);

	// Inactive cliff carving must not convert an enormous but finite
	// surface directly to an integer before applying the generation bounds.
	params.spflags = MGVALLEYS_CARVE_CLIFFS;
	params.np_terrain_height.offset = 1.0e20f;
	params.np_rivers.offset = 2.0f;
	auto beyond_world = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, beyond_world->sampleHeight(v2s16(-17, 31)),
			(float)MAX_MAP_GENERATION_LIMIT);
	params.np_terrain_height.offset = 20.0f;
	params.np_rivers.offset = 1.0f;

	params.spflags = MGVALLEYS_MOUNTAINS;
	params.np_mountain_height.offset = 40.0f;
	params.np_mountain_height.scale = 0.0f;
	params.np_mountain.offset = 1.0f;
	params.np_mountain.scale = 0.0f;
	auto mountains = createValleysBiomeTerrainSampler(params);
	UASSERT(mountains->sampleHeight(v2s16(-17, 31)) > 29.0f);

	// The cap has to raise the modeled surface beyond the mountain body.
	// A general "mountains are higher" assertion would miss an omitted cap.
	params.np_mountain.offset = 0.1f;
	params.np_mountain_height.offset = 20.0f;
	params.mountain_cap = 0.0f;
	auto body = createValleysBiomeTerrainSampler(params);
	const float body_surface = body->sampleHeight(v2s16(-17, 31));
	params.mountain_cap = 10.0f;
	auto capped = createValleysBiomeTerrainSampler(params);
	UASSERT(capped->sampleHeight(v2s16(-17, 31)) > body_surface + 5.0f);
	params.spflags = 0;
	UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(v2s16(-17, 31)),
			19.0f);
	params.spflags = MGVALLEYS_MOUNTAINS;
	for (float mask : {0.0f, -20.0f}) {
		params.np_mountain_height.offset = mask;
		UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(v2s16(-17, 31)),
				19.0f);
	}
	params.np_mountain_height.offset = 20.0f;
	params.np_mountain.offset = -0.1f;
	UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(v2s16(-17, 31)),
			19.0f);

	// At the centre of this constant river the bed is nine nodes below
	// the bank. Sea-level channels lower the bank from 20 to 2; their
	// bed must be allowed below the ordinary water_level - 3 clamp.
	params.spflags = 0;
	params.np_rivers.offset = 0.0f;
	params.river_depth = 8;
	auto elevated_river = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, elevated_river->sampleHeight(v2s16(-17, 31)), 10.0f);
	params.spflags = MGVALLEYS_SEA_LEVEL_RIVERS;
	auto sea_river = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, sea_river->sampleHeight(v2s16(-17, 31)), -8.0f);
	params.floor_y = -6;
	auto floored_river = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, floored_river->sampleHeight(v2s16(-17, 31)), -6.0f);

	// A nonconstant surface must be independent of sampling order, cache
	// lifetime, and the worker that samples it, including across x/z = 0.
	MapgenValleysParams varied_params;
	varied_params.seed = 54321;
	varied_params.spflags = MGVALLEYS_MOUNTAINS;
	auto varied = createValleysBiomeTerrainSampler(varied_params);
	const v2s16 positions[] = {
		v2s16(-17, -9), v2s16(-1, 0), v2s16(0, -1), v2s16(15, 16),
		v2s16(-MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT),
		v2s16(MAX_MAP_GENERATION_LIMIT, -MAX_MAP_GENERATION_LIMIT)
	};
	std::vector<float> expected;
	for (auto pos : positions) {
		const float height = varied->sampleHeight(pos);
		UASSERT(std::isfinite(height));
		expected.push_back(height);
	}
	auto clone = varied->clone();
	for (size_t i = expected.size(); i-- > 0;) {
		UASSERTEQ(float, varied->sampleHeight(positions[i]), expected[i]);
		UASSERTEQ(float, clone->sampleHeight(positions[i]), expected[i]);
	}
}

void TestMapgen::testBiomeTerrainProfile()
{
	MapgenValleysParams params;
	params.seed = 14413353056704472440ULL;
	params.chunksize = v3s16(5);
	params.spflags = MGVALLEYS_MOUNTAINS | MGVALLEYS_SEA_LEVEL_RIVERS;
	params.water_level = 0;
	params.floor_y = -61;
	params.river_size = 14;
	params.river_depth = 6;
	params.river_valley_width = 0.5f;
	params.np_inter_valley_fill = {0, 1, v3f(256, 512, 256), 1993, 6, 0.55f, 2};
	params.np_inter_valley_slope = {0.25f, 0.15f, v3f(128), 746, 1, 1, 2};
	params.np_rivers = {0, 1, v3f(256), -6050, 5, 0.6f, 2};
	params.np_terrain_height = {6, 50, v3f(1024), 5202, 6, 0.4f, 2};
	params.np_valley_depth = {2.6f, 2, v3f(512), -1914, 1, 1, 2};
	params.np_valley_profile = {1.5f, 0.5f, v3f(512), 777, 1, 1, 2};
	params.np_mountain = {-0.55f, 1, v3f(192, 256, 192), 3517, 5, 0.7f, 2};
	params.np_mountain_height = {-30, 280, v3f(800), 4021, 3, 0.6f, 2};
	auto raw = createValleysBiomeTerrainSampler(params);

	// The unconnected mountain cap at this column is removed by natural
	// floater cleanup. Its height must not be reported as the ground.
	const v2s16 floater_pos(744, -1584);
	UASSERTEQ(float, raw->sampleHeight(floater_pos), 185.0f);
	params.spflags |= MGVALLEYS_REMOVE_FLOATERS;
	auto cleaned = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, cleaned->sampleHeight(floater_pos), 104.0f);

	// These heights come from native terrain generation with caves and
	// biome-material replacement disabled, before and after cliff carving.
	const v2s16 cliff_pos(680, -1584);
	UASSERTEQ(float, cleaned->sampleHeight(cliff_pos), 24.0f);
	params.spflags |= MGVALLEYS_CARVE_CLIFFS;
	auto carved = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, carved->sampleHeight(cliff_pos), 13.0f);

	// Cleanup operates on canonical chunks, including their Y extent.
	// The first pairs straddle X/Z chunk boundaries; the final pair probes
	// the world edges with cleanup enabled. Different chunk sizes
	// may retain different components, but query order and worker state
	// must never change the result for a given size.
	struct CleanupCase {
		v3s16 chunksize;
		v2s16 positions[6];
	};
	const CleanupCase cases[] = {
		{v3s16(5), {v2s16(680, -1584), v2s16(688, -1584),
			v2s16(744, -1560), v2s16(744, -1552),
			v2s16(-MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT),
			v2s16(MAX_MAP_GENERATION_LIMIT, -MAX_MAP_GENERATION_LIMIT)}},
		{v3s16(3, 2, 4), {v2s16(696, -1584), v2s16(704, -1584),
			v2s16(744, -1576), v2s16(744, -1568),
			v2s16(-MAX_MAP_GENERATION_LIMIT, MAX_MAP_GENERATION_LIMIT),
			v2s16(MAX_MAP_GENERATION_LIMIT, -MAX_MAP_GENERATION_LIMIT)}}
	};
	for (const auto &test : cases) {
		params.chunksize = test.chunksize;
		auto sampler = createValleysBiomeTerrainSampler(params);
		std::vector<float> expected;
		for (auto pos : test.positions)
			expected.push_back(sampler->sampleHeight(pos));
		auto clone = sampler->clone();
		for (size_t i = expected.size(); i-- > 0;) {
			UASSERT(std::isfinite(expected[i]));
			UASSERTEQ(float, sampler->sampleHeight(test.positions[i]), expected[i]);
			UASSERTEQ(float, clone->sampleHeight(test.positions[i]), expected[i]);
		}
	}
}
void TestMapgen::testBiomeTerrainFloaterSeeds()
{
	// Constant 2D noises whose valley term rounds to exactly 1 (surface 48,
	// floor 42, slope 4) and one octave of white 3D noise at spread 1 keep
	// every value platform-exact: a node is solid where 4 * n_fill > y - 48.
	// The expected heights come from a transcription of
	// MapgenValleys::removeFloaters applied to the same modeled field.
	auto floaterParams = [](u64 seed) {
		MapgenValleysParams params;
		params.seed = seed;
		params.chunksize = v3s16(5);
		params.spflags = MGVALLEYS_REMOVE_FLOATERS;
		params.water_level = 0;
		params.floor_y = -61;
		params.np_terrain_height = {40, 0, v3f(1024), 5202, 1, 1, 2};
		params.np_valley_depth = {2, 0, v3f(512), -1914, 1, 1, 2};
		params.np_valley_profile = {1, 0, v3f(512), 777, 1, 1, 2};
		params.np_rivers = {5, 0, v3f(256), -6050, 1, 1, 2};
		params.np_inter_valley_slope = {1, 0, v3f(128), 746, 1, 1, 2};
		params.np_inter_valley_fill = {-1.2f, 1, v3f(1, 1, 1), 1993, 1, 1, 2};
		return params;
	};
	struct Column {
		u64 seed;
		v2s16 pos;
		float height;
	};

	// The topmost run of solid nodes in these columns reaches the level just
	// above the floor, with air below it. The native fill does not start
	// from such a top, but a neighbouring column seeds the same floating
	// piece, so it goes. Taking the run for ground would report its top.
	const Column removed[] = {
		{14, v2s16(40, 8), 40}, {27, v2s16(-24, 0), 42}, {35, v2s16(40, 0), 42}
	};
	for (const auto &column : removed) {
		auto params = floaterParams(column.seed);
		UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(
				column.pos), column.height);
		params.spflags = 0;
		UASSERT(createValleysBiomeTerrainSampler(params)->sampleHeight(
				column.pos) > column.height);
	}

	// Every column top of these floating pieces has such a run, so the
	// native fill never starts on them and they stay: a lower top under a
	// column, or the top itself where nothing floats above it.
	const Column kept[] = {
		{216, v2s16(104, 0), 45}, {220, v2s16(80, 16), 44}, {309, v2s16(104, -8), 44}
	};
	for (const auto &column : kept) {
		auto params = floaterParams(column.seed);
		UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sampleHeight(
				column.pos), column.height);
		params.spflags = 0;
		UASSERT(createValleysBiomeTerrainSampler(params)->sampleHeight(
				column.pos) >= column.height);
	}
}

void TestMapgen::testValleysSurfaceModel(IGameDef *gamedef)
{
	// The biome terrain sampler models the natural surface of one column
	// on its own: the density with the mountain body and its cap, the
	// solid floor, the cliff carving and the floating piece removal, each
	// written a second time for a column instead of a mapchunk. Generate
	// stacks of mapchunks with the game's profile, terrain only, and
	// compare the top solid node of every column with the model: any
	// change to one side that the other does not follow shows up here.
	MockServer server(getTestTempDirectory());
	NodeDefManager ndef;
	auto add_node = [&](const char *name, content_t source, bool walkable) {
		ContentFeatures def = gamedef->ndef()->get(source);
		def.name = name;
		def.walkable = walkable;
		return ndef.set(name, def);
	};
	const content_t stone = add_node("mapgen_stone", t_CONTENT_STONE, true);
	add_node("mapgen_water_source", t_CONTENT_WATER, false);
	add_node("mapgen_river_water_source", t_CONTENT_WATER, false);
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(&ndef);
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	server.ndef()->cancelNodeResolveCallback(default_biome);
	ndef.pendNodeResolve(default_biome);
	ndef.setNodeRegistrationStatus(true);
	ndef.runNodeResolveCallbacks();

	MapgenValleysParams params;
	params.seed = 14413353056704472440ULL;
	params.chunksize = v3s16(5);
	params.flags = 0;
	params.spflags = MGVALLEYS_MOUNTAINS | MGVALLEYS_SEA_LEVEL_RIVERS |
		MGVALLEYS_CARVE_CLIFFS | MGVALLEYS_REMOVE_FLOATERS;
	params.water_level = 0;
	params.floor_y = -61;
	params.river_size = 14;
	params.river_depth = 6;
	params.river_valley_width = 0.5f;
	params.np_inter_valley_fill = {0, 1, v3f(256, 512, 256), 1993, 6, 0.55f, 2};
	params.np_inter_valley_slope = {0.25f, 0.15f, v3f(128), 746, 1, 1, 2};
	params.np_rivers = {0, 1, v3f(256), -6050, 5, 0.6f, 2};
	params.np_terrain_height = {6, 50, v3f(1024), 5202, 6, 0.4f, 2};
	params.np_valley_depth = {2.6f, 2, v3f(512), -1914, 1, 1, 2};
	params.np_valley_profile = {1.5f, 0.5f, v3f(512), 777, 1, 1, 2};
	params.np_mountain = {-0.55f, 1, v3f(192, 256, 192), 3517, 5, 0.7f, 2};
	params.np_mountain_height = {-30, 280, v3f(800), 4021, 3, 0.6f, 2};
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	MetricsBackend metrics;
	EmergeManager emerge(&server, &metrics);
	emerge.ndef = &ndef;
	BiomeGenOriginal source(&manager, &climate, v3s16(5 * MAP_BLOCKSIZE));
	MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
		emerge.getOreManager(), emerge.getDecorationManager(),
		emerge.getSchematicManager()));
	auto sampler = createValleysBiomeTerrainSampler(params);

	// The two mapchunk columns of the profile fixtures, the carved cliff
	// and the removed mountain cap, in mapblocks on the chunk grid, and a
	// stack from the solid floor to above the highest body
	const v3s16 chunk_blocks = params.chunksize;
	const s16 side = chunk_blocks.X * MAP_BLOCKSIZE;
	const v2s16 stacks[] = {v2s16(38, -102), v2s16(43, -102)};
	const s16 lowest_block = -7;
	const s16 highest_block = 18;
	size_t mismatches = 0;
	size_t columns = 0;
	for (const auto &stack : stacks) {
		std::vector<s16> top((size_t)side * side, -MAX_MAP_GENERATION_LIMIT);
		for (s16 block_y = lowest_block; block_y <= highest_block;
				block_y += chunk_blocks.Y) {
			BlockMakeData data;
			data.blockpos_min = v3s16(stack.X, block_y, stack.Y);
			data.blockpos_max = data.blockpos_min + chunk_blocks - v3s16(1);
			data.seed = params.seed;
			data.nodedef = &ndef;
			const v3s16 node_min = data.blockpos_min * MAP_BLOCKSIZE;
			const v3s16 node_max = (data.blockpos_max + v3s16(1)) * MAP_BLOCKSIZE - v3s16(1);
			data.vmanip = new MapgenTestVManip(VoxelArea(
				node_min - v3s16(MAP_BLOCKSIZE), node_max + v3s16(MAP_BLOCKSIZE)));
			mapgen.makeChunk(&data);
			for (s16 z = node_min.Z; z <= node_max.Z; ++z)
			for (s16 x = node_min.X; x <= node_max.X; ++x) {
				s16 &column_top = top[(size_t)(z - node_min.Z) * side + (x - node_min.X)];
				for (s16 y = node_max.Y; y >= node_min.Y && y > column_top; --y) {
					if (data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)]
							.getContent() == stone) {
						column_top = y;
						break;
					}
				}
			}
		}
		const s16 node_min_x = stack.X * MAP_BLOCKSIZE;
		const s16 node_min_z = stack.Y * MAP_BLOCKSIZE;
		const s16 ceiling = (highest_block + chunk_blocks.Y) * MAP_BLOCKSIZE - 1;
		for (s16 z = 0; z < side; ++z)
		for (s16 x = 0; x < side; ++x) {
			const v2s16 pos(node_min_x + x, node_min_z + z);
			const float modeled = sampler->sampleHeight(pos);
			const s16 generated = top[(size_t)z * side + x];
			UASSERT(std::isfinite(modeled) && modeled < ceiling);
			UASSERT(generated >= params.floor_y);
			++columns;
			if (modeled != static_cast<float>(generated)) {
				if (mismatches < 8)
					errorstream << "Valleys surface model: column " << pos
						<< " modeled " << modeled << ", generated "
						<< generated << std::endl;
				++mismatches;
			}
		}
	}
	UASSERTEQ(size_t, columns, 2 * (size_t)side * side);
	UASSERTEQ(size_t, mismatches, 0);
	// The fixtures of testBiomeTerrainProfile lie in these stacks, the
	// carved cliff and the removed cap: the area compared is not a flat one
	UASSERTEQ(float, sampler->sampleHeight(v2s16(680, -1584)), 13.0f);
	UASSERTEQ(float, sampler->sampleHeight(v2s16(744, -1584)), 104.0f);
}

void TestMapgen::testValleysFloaterBiomes(IGameDef *gamedef)
{
	// Two biomes on one climate point, told apart by their Y bands, and a
	// block of stone set in the vmanip before generation, which the terrain
	// pass leaves in place, hanging over the ground or the sea. The biome
	// pass records the biome of the block's top for its columns; after the
	// floating piece removal the biomemap must name the biome of the
	// surface each column has left, selected as the pass selects it, and
	// the nodes stay as laid.
	MockServer server(getTestTempDirectory());
	NodeDefManager ndef;
	auto add_node = [&](const char *name, content_t source, bool walkable) {
		ContentFeatures def = gamedef->ndef()->get(source);
		def.name = name;
		def.walkable = walkable;
		return ndef.set(name, def);
	};
	const content_t stone = add_node("mapgen_stone", t_CONTENT_STONE, true);
	const content_t water = add_node("mapgen_water_source", t_CONTENT_WATER, false);
	const content_t river = add_node("mapgen_river_water_source", t_CONTENT_WATER, false);
	// A walkable lid the biome pass lays on the water of a frozen sea
	const content_t ice = add_node("test:ice", t_CONTENT_STONE, true);
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(&ndef);
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	server.ndef()->cancelNodeResolveCallback(default_biome);
	ndef.pendNodeResolve(default_biome);
	auto add_biome = [&](const char *name) {
		std::string top_name = std::string("test:") + name + "_top";
		const content_t top = add_node(top_name.c_str(), t_CONTENT_GRASS, true);
		auto biome = addTerrainTestBiome(manager, name, 50.0f, 50.0f);
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		biome->m_nodenames[0] = top_name;
		ndef.pendNodeResolve(biome);
		biome->c_top = top;
		biome->depth_top = 1;
		biome->c_filler = biome->c_stone = stone;
		biome->depth_filler = 0;
		biome->c_water = biome->c_water_top = water;
		biome->depth_water_top = 1;
		biome->c_river_water = river;
		biome->c_riverbed = stone;
		return biome;
	};
	auto ground = add_biome("ground");
	auto high = add_biome("high");
	ndef.setNodeRegistrationStatus(true);
	ndef.runNodeResolveCallbacks();

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	// The ridge column of the form fixture: surface 32 and slope 16, so
	// stone up to Y 52 and a floater floor at 8
	MapgenValleysParams params;
	params.seed = 12345;
	params.chunksize = v3s16(1);
	params.flags = MG_BIOMES;
	params.spflags = MGVALLEYS_REMOVE_FLOATERS;
	params.altitude_chill = 100;
	params.mountain_cap = 0.0f;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	params.np_filler_depth = constant_noise(0.0f);
	params.np_mountain = constant_noise(1.0f);
	params.np_mountain_height = constant_noise(20.0f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	MetricsBackend metrics;
	EmergeManager emerge(&server, &metrics);
	emerge.ndef = &ndef;

	const s16 none = -MAX_MAP_GENERATION_LIMIT;
	const biome_t ground_id = ground->index;
	const biome_t high_id = high->index;
	const struct {
		const char *name;
		s16 block_y;
		s16 water_level;
		bool frozen;        // the water top is the ice lid
		s16 ground_max_y;   // 'ground' ends here, 'high' starts above it
		v3s16 piece_min;    // the block of stone, relative to node_min
		v3s16 piece_max;
		bool removed;
		biome_t under;      // biomemap of the piece's columns afterwards
		s16 under_top;      // their heightmap afterwards, absolute
		biome_t outside;    // biomemap of every other column
		s16 outside_top;
	} cases[] = {
		// The piece over the ground goes; the ground wears its own biome
		{"removed over the ground", 3, 0, false, 55,
			v3s16(2, 10, 2), v3s16(5, 11, 5), true, ground_id, 52, ground_id, 52},
		// A piece on the mapchunk edge is grounded and stays, biome and all
		{"kept at the edge", 3, 0, false, 55,
			v3s16(0, 10, 2), v3s16(3, 11, 5), false, high_id, 59, ground_id, 52},
		// Nothing solid left in the mapchunk: no biome
		{"removed over nothing", 4, 0, false, 55,
			v3s16(2, 6, 2), v3s16(5, 7, 5), true, BIOME_NONE, none, BIOME_NONE, none},
		// Over the sea the column falls back to the biome of the water
		// surface, as the biome pass gives open water
		{"removed over the sea", 4, 70, false, 72,
			v3s16(2, 10, 2), v3s16(5, 11, 5), true, ground_id, none, ground_id, none},
		// The ice lid tops the column but is not its ground: with the bed
		// below the mapchunk the lid is the liquid surface
		{"removed over a frozen sea", 4, 70, true, 72,
			v3s16(2, 10, 2), v3s16(5, 11, 5), true, ground_id, 70, ground_id, 70},
		// ... and with the bed in the mapchunk the biome is the bed's, as the
		// biome pass selects it, not the lid's
		{"removed over a frozen bed", 3, 60, true, 55,
			v3s16(2, 14, 2), v3s16(5, 14, 5), true, ground_id, 60, ground_id, 60},
	};
	for (const auto &test : cases) {
		infostream << "Valleys floater fixture: " << test.name << std::endl;
		params.water_level = test.water_level;
		ground->max_pos.Y = test.ground_max_y;
		high->min_pos.Y = test.ground_max_y + 1;
		ground->c_water_top = high->c_water_top = test.frozen ? ice : water;
		BiomeGenOriginal source(&manager, &climate, v3s16(MAP_BLOCKSIZE));
		source.setValleysClimate(params);
		MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
			emerge.getOreManager(), emerge.getDecorationManager(),
			emerge.getSchematicManager()));
		const v3s16 node_min(-16, test.block_y * MAP_BLOCKSIZE, 16);
		const v3s16 node_max = node_min + v3s16(MAP_BLOCKSIZE - 1);
		const v3s16 piece_min = node_min + test.piece_min;
		const v3s16 piece_max = node_min + test.piece_max;
		BlockMakeData data;
		data.blockpos_min = data.blockpos_max = v3s16(-1, test.block_y, 1);
		data.seed = params.seed;
		data.nodedef = &ndef;
		data.vmanip = new MapgenTestVManip(VoxelArea(
			node_min - v3s16(MAP_BLOCKSIZE), node_max + v3s16(MAP_BLOCKSIZE)));
		auto node_at = [&](s16 x, s16 y, s16 z) -> MapNode & {
			return data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)];
		};
		for (s16 z = piece_min.Z; z <= piece_max.Z; ++z)
		for (s16 y = piece_min.Y; y <= piece_max.Y; ++y)
		for (s16 x = piece_min.X; x <= piece_max.X; ++x)
			node_at(x, y, z) = MapNode(stone);
		mapgen.makeChunk(&data);

		for (s16 z = node_min.Z; z <= node_max.Z; ++z)
		for (s16 x = node_min.X; x <= node_max.X; ++x) {
			const size_t index = (z - node_min.Z) * MAP_BLOCKSIZE + x - node_min.X;
			const bool under = x >= piece_min.X && x <= piece_max.X &&
				z >= piece_min.Z && z <= piece_max.Z;
			const s16 top = under ? test.under_top : test.outside_top;
			UASSERTEQ(s16, mapgen.heightmap[index], top);
			UASSERTEQ(biome_t, mapgen.biomemap[index], under ? test.under : test.outside);
			if (top == none)
				continue;
			// The top node is the lid, or wears the biome the biomemap names
			auto biome = static_cast<Biome *>(manager.getRaw(mapgen.biomemap[index]));
			const content_t top_node = node_at(x, top, z).getContent();
			if (test.frozen && top == test.water_level) {
				UASSERTEQ(content_t, top_node, ice);
			} else {
				UASSERTEQ(content_t, top_node, biome->c_top);
			}
		}
		for (s16 z = piece_min.Z; z <= piece_max.Z; ++z)
		for (s16 y = piece_min.Y; y <= piece_max.Y; ++y)
		for (s16 x = piece_min.X; x <= piece_max.X; ++x) {
			const content_t c = node_at(x, y, z).getContent();
			if (test.removed) {
				UASSERTEQ(content_t, c, CONTENT_AIR);
			} else {
				UASSERT(c == high->c_top || c == stone);
			}
		}
	}
}

void TestMapgen::testDecorationBiomeAtSurface(IGameDef *gamedef)
{
	// Two biomes on one climate point, stacked by Y, and a decoration
	// bound to one of them, judged by the column's biomemap entry or by the
	// biome at the surface it stands on.
	MockServer server(getTestTempDirectory());
	NodeDefManager ndef;
	auto add_node = [&](const char *name, content_t source, bool walkable) {
		ContentFeatures def = gamedef->ndef()->get(source);
		def.name = name;
		def.walkable = walkable;
		return ndef.set(name, def);
	};
	const content_t stone = add_node("mapgen_stone", t_CONTENT_STONE, true);
	const content_t water = add_node("mapgen_water_source", t_CONTENT_WATER, false);
	const content_t river = add_node("mapgen_river_water_source", t_CONTENT_WATER, false);
	const content_t glow = add_node("test:glow", t_CONTENT_GRASS, true);
	const content_t lily = add_node("test:lily", t_CONTENT_GRASS, true);
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(&ndef);
	auto default_biome = static_cast<Biome *>(manager.getRaw(BIOME_NONE));
	server.ndef()->cancelNodeResolveCallback(default_biome);
	ndef.pendNodeResolve(default_biome);
	// All stone: no top or filler, so a surface under air stays stone
	auto add_biome = [&](const char *name) {
		auto biome = addTerrainTestBiome(manager, name, 50.0f, 50.0f);
		biome->m_nodenames = default_biome->m_nodenames;
		biome->m_nnlistsizes = default_biome->m_nnlistsizes;
		ndef.pendNodeResolve(biome);
		biome->c_top = biome->c_filler = biome->c_stone = stone;
		biome->depth_top = 0;
		biome->depth_filler = 0;
		biome->c_water = biome->c_water_top = water;
		biome->c_river_water = river;
		biome->c_riverbed = stone;
		return biome;
	};
	auto lower = add_biome("lower");
	auto upper = add_biome("upper");
	ndef.setNodeRegistrationStatus(true);
	ndef.runNodeResolveCallbacks();

	auto constant_noise = [](float value) {
		return NoiseParams(value, 0.0f, v3f(64.0f), 0, 1, 0.5f, 2.0f);
	};
	// The ridge column of the form fixture: stone up to Y 52
	MapgenValleysParams params;
	params.seed = 12345;
	params.chunksize = v3s16(1);
	params.spflags = 0;
	params.altitude_chill = 100;
	params.mountain_cap = 0.0f;
	params.np_terrain_height = constant_noise(0.0f);
	params.np_valley_depth = constant_noise(4.0f);
	params.np_valley_profile = constant_noise(1.0f);
	params.np_rivers = constant_noise(100.0f);
	params.np_inter_valley_slope = constant_noise(1.0f);
	params.np_inter_valley_fill = constant_noise(1.3125f);
	params.np_filler_depth = constant_noise(0.0f);
	params.np_mountain = constant_noise(1.0f);
	params.np_mountain_height = constant_noise(20.0f);
	BiomeParamsOriginal climate;
	climate.seed = params.seed;
	climate.np_heat = climate.np_humidity = constant_noise(50.0f);
	climate.np_heat_blend = climate.np_humidity_blend = constant_noise(0.0f);
	MetricsBackend metrics;
	EmergeManager emerge(&server, &metrics);
	emerge.ndef = &ndef;

	// One node on every surface of every column the filter admits
	auto add_deco = [&](DecorationManager &decorations, const char *name,
			content_t place_on, content_t node) {
		auto deco = static_cast<DecoSimple *>(DecorationManager::create(DECO_SIMPLE));
		deco->name = name;
		deco->c_place_on.push_back(place_on);
		deco->c_decos.push_back(node);
		deco->deco_height = 1;
		deco->deco_height_max = 0;
		deco->deco_param2 = 0;
		deco->deco_param2_max = 0;
		deco->sidelen = MAP_BLOCKSIZE;
		deco->fill_ratio = 10.0f;
		deco->y_min = -MAX_MAP_GENERATION_LIMIT;
		deco->y_max = MAX_MAP_GENERATION_LIMIT;
		deco->nspawnby = -1;
		UASSERT(decorations.add(deco) != OBJDEF_INVALID_HANDLE);
		return deco;
	};
	auto make_chunk = [&](MapgenValleys &mapgen, BlockMakeData &data, s16 block_y) {
		const v3s16 node_min(-16, block_y * MAP_BLOCKSIZE, 16);
		data.blockpos_min = data.blockpos_max = v3s16(-1, block_y, 1);
		data.seed = params.seed;
		data.nodedef = &ndef;
		data.vmanip = new MapgenTestVManip(VoxelArea(node_min - v3s16(MAP_BLOCKSIZE),
			node_min + v3s16(2 * MAP_BLOCKSIZE - 1)));
		return node_min;
	};

	// In the stone. The mapblock at Y 16..31 is solid, with an air pocket
	// set in each biome before generation, which the terrain pass leaves
	// in place: Y 18..20 in the lower biome, which ends at 23, and 26..28
	// in the upper. The biomemap entry is the biome of the mapchunk's top,
	// so by it both pockets are the upper biome's; at the surface each is
	// its own biome's. A vertical blend of the lower biome reaches the
	// node under the upper pocket, Y 25: as the first floor of its run it is
	// dithered, and this climate draws the lower biome there; after a
	// higher floor of the same run, the floor of a third pocket at Y 30, it
	// keeps the biome selected up there, as the biome pass would.
	{
		DecorationManager decorations(&server);
		auto deco = add_deco(decorations, "test:glow", stone, glow);
		const s16 lower_floor = 18, lower_ceiling = 20;
		const s16 upper_floor = 26, upper_ceiling = 28;
		const s16 top_pocket = 30;
		const u32 surfaces = DECO_ALL_FLOORS | DECO_ALL_CEILINGS;
		const u32 at_surface = surfaces | DECO_BIOME_AT_SURFACE;
		const u32 floors_at_surface = DECO_ALL_FLOORS | DECO_BIOME_AT_SURFACE;
		const s16 blend = 3;
		{
			// The selector's dither for the node at 25, two above the lower
			// biome, in a climate of 50 and 50
			PcgRandom rng(static_cast<s64>(25 + (50.0f + 50.0f) * 0.9f));
			UASSERT(rng.range(0, blend) >= 25 - 23);
		}
		enum Lined : u8 { NONE = 0, FLOOR = 1, CEILING = 2, BOTH = 3 };
		const struct {
			const char *name;
			u32 mg_flags;
			u32 flags;
			Biome *bound;
			s16 blend;          // of the lower biome
			bool third_pocket;
			s16 y_min, y_max;   // of the decoration
			u8 lower_lined;
			u8 upper_lined;
		} cases[] = {
			{"lower by the biomemap", MG_BIOMES, surfaces, lower, 0, false,
				-100, 100, NONE, NONE},
			{"upper by the biomemap", MG_BIOMES, surfaces, upper, 0, false,
				-100, 100, BOTH, BOTH},
			{"lower at the surface", MG_BIOMES, at_surface, lower, 0, false,
				-100, 100, BOTH, NONE},
			{"upper at the surface", MG_BIOMES, at_surface, upper, 0, false,
				-100, 100, NONE, BOTH},
			// The Y range cuts the scan, not what is found in the range:
			// the lower pocket's ceiling stands on the node at 21, the
			// upper pocket's floor on the node at 25
			{"upper at the surface, floors of the range", MG_BIOMES, at_surface, upper,
				0, false, 21, 25, NONE, FLOOR},
			{"lower at the surface, ceilings of the range", MG_BIOMES, at_surface, lower,
				0, false, 21, 25, CEILING, NONE},
			// The blend: the first floor of a run is dithered, a later one
			// keeps the biome of the run
			{"blended floor, first of its run", MG_BIOMES, floors_at_surface, lower,
				blend, false, -100, 100, FLOOR, FLOOR},
			{"blended floor, after a higher one", MG_BIOMES, floors_at_surface, lower,
				blend, true, -100, 100, FLOOR, NONE},
			// The mapgen's biomes off: no climate of this mapchunk to
			// select by, and a biomemap that names no biome
			{"biomes off", 0, at_surface, lower, 0, false,
				-100, 100, NONE, NONE},
		};
		for (const auto &test : cases) {
			infostream << "Decoration biome fixture: " << test.name << std::endl;
			params.flags = test.mg_flags | MG_DECORATIONS;
			params.water_level = 0;
			lower->max_pos.Y = 23;
			lower->vertical_blend = test.blend;
			upper->min_pos.Y = 24;
			deco->flags = test.flags;
			deco->y_min = test.y_min;
			deco->y_max = test.y_max;
			deco->biomes.clear();
			deco->biomes.insert(test.bound->index);
			BiomeGenOriginal source(&manager, &climate, v3s16(MAP_BLOCKSIZE));
			source.setValleysClimate(params);
			MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
				emerge.getOreManager(), &decorations, emerge.getSchematicManager()));
			BlockMakeData data;
			const v3s16 node_min = make_chunk(mapgen, data, 1);
			const v3s16 node_max = node_min + v3s16(MAP_BLOCKSIZE - 1);
			const v2s16 pocket_min(node_min.X + 2, node_min.Z + 2);
			const v2s16 pocket_max(node_min.X + 5, node_min.Z + 5);
			auto node_at = [&](s16 x, s16 y, s16 z) -> MapNode & {
				return data.vmanip->m_data[data.vmanip->m_area.index(x, y, z)];
			};
			for (s16 z = pocket_min.Y; z <= pocket_max.Y; ++z)
			for (s16 x = pocket_min.X; x <= pocket_max.X; ++x) {
				for (s16 y = lower_floor; y <= lower_ceiling; ++y)
					node_at(x, y, z) = MapNode(CONTENT_AIR);
				for (s16 y = upper_floor; y <= upper_ceiling; ++y)
					node_at(x, y, z) = MapNode(CONTENT_AIR);
				if (test.third_pocket)
					node_at(x, top_pocket, z) = MapNode(CONTENT_AIR);
			}
			mapgen.makeChunk(&data);

			const biome_t column_biome = (test.mg_flags & MG_BIOMES) ?
				upper->index : BIOME_NONE;
			for (s16 z = node_min.Z; z <= node_max.Z; ++z)
			for (s16 x = node_min.X; x <= node_max.X; ++x) {
				const size_t index = (z - node_min.Z) * MAP_BLOCKSIZE + x - node_min.X;
				UASSERTEQ(biome_t, mapgen.biomemap[index], column_biome);
			}
			auto expected = [&](u8 lined, u8 which) {
				return (lined & which) ? glow : (content_t)CONTENT_AIR;
			};
			for (s16 z = node_min.Z; z <= node_max.Z; ++z)
			for (s16 y = node_min.Y; y <= node_max.Y; ++y)
			for (s16 x = node_min.X; x <= node_max.X; ++x) {
				const bool in_column = x >= pocket_min.X && x <= pocket_max.X &&
					z >= pocket_min.Y && z <= pocket_max.Y;
				content_t want = stone;
				if (in_column && y >= lower_floor && y <= lower_ceiling) {
					want = y == lower_floor ? expected(test.lower_lined, FLOOR) :
						y == lower_ceiling ? expected(test.lower_lined, CEILING) :
						(content_t)CONTENT_AIR;
				} else if (in_column && y >= upper_floor && y <= upper_ceiling) {
					want = y == upper_floor ? expected(test.upper_lined, FLOOR) :
						y == upper_ceiling ? expected(test.upper_lined, CEILING) :
						(content_t)CONTENT_AIR;
				} else if (in_column && test.third_pocket && y == top_pocket) {
					// Its floor is the upper biome's, which no case here
					// binds together with a third pocket
					want = CONTENT_AIR;
				}
				UASSERTEQ(content_t, node_at(x, y, z).getContent(), want);
			}
		}
	}

	// On the water. The mapblock at Y 48..63 holds the bed at 52, in the
	// lower biome, which ends at 56, and the water line at 60, in the
	// upper. The biomemap entry is the bed's biome; a decoration on the
	// liquid surface stands on the water.
	{
		DecorationManager decorations(&server);
		auto deco = add_deco(decorations, "test:lily", water, lily);
		const s16 water_level = 60;
		const struct {
			const char *name;
			u32 flags;
			Biome *bound;
			bool placed;
		} cases[] = {
			{"bed by the biomemap", DECO_LIQUID_SURFACE, lower, true},
			{"water line by the biomemap", DECO_LIQUID_SURFACE, upper, false},
			{"bed at the surface",
				DECO_LIQUID_SURFACE | DECO_BIOME_AT_SURFACE, lower, false},
			{"water line at the surface",
				DECO_LIQUID_SURFACE | DECO_BIOME_AT_SURFACE, upper, true},
		};
		for (const auto &test : cases) {
			infostream << "Decoration biome fixture: " << test.name << std::endl;
			params.flags = MG_BIOMES | MG_DECORATIONS;
			params.water_level = water_level;
			lower->max_pos.Y = 56;
			lower->vertical_blend = 0;
			upper->min_pos.Y = 57;
			deco->flags = test.flags;
			deco->biomes.clear();
			deco->biomes.insert(test.bound->index);
			BiomeGenOriginal source(&manager, &climate, v3s16(MAP_BLOCKSIZE));
			source.setValleysClimate(params);
			MapgenValleys mapgen(&params, new EmergeParams(&emerge, &source, &manager,
				emerge.getOreManager(), &decorations, emerge.getSchematicManager()));
			BlockMakeData data;
			const v3s16 node_min = make_chunk(mapgen, data, 3);
			const v3s16 node_max = node_min + v3s16(MAP_BLOCKSIZE - 1);
			mapgen.makeChunk(&data);

			for (s16 z = node_min.Z; z <= node_max.Z; ++z)
			for (s16 x = node_min.X; x <= node_max.X; ++x) {
				const size_t index = (z - node_min.Z) * MAP_BLOCKSIZE + x - node_min.X;
				UASSERTEQ(biome_t, mapgen.biomemap[index], lower->index);
				auto node_at = [&](s16 y) {
					return data.vmanip->m_data[
						data.vmanip->m_area.index(x, y, z)].getContent();
				};
				UASSERTEQ(content_t, node_at(water_level), water);
				UASSERTEQ(content_t, node_at(water_level + 1),
					test.placed ? lily : (content_t)CONTENT_AIR);
				UASSERTEQ(content_t, node_at(water_level + 2), CONTENT_AIR);
			}
		}
	}
}

#endif
