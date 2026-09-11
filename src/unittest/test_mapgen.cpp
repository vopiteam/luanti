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
#include "script/common/c_types.h"
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
	void testBiomeTerrainRanges();
	void testBiomeTerrainParsing();
	void testBiomeTerrainSelection(IGameDef *gamedef);
	void testBiomeTerrainBlending(IGameDef *gamedef);
	void testBiomeTerrainClone(IGameDef *gamedef);
	void testBiomeTerrainMetrics();
	void testBiomeTerrainFolds();
	void testBiomeTerrainValleys();
	void testBiomeTerrainProfile();
	void testBiomeTerrainFloaterSeeds();
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
	TEST(testBiomeTerrainRanges);
	TEST(testBiomeTerrainParsing);
	TEST(testBiomeTerrainSelection, gamedef);
	TEST(testBiomeTerrainBlending, gamedef);
	TEST(testBiomeTerrainClone, gamedef);
	TEST(testBiomeTerrainMetrics);
	TEST(testBiomeTerrainFolds);
	TEST(testBiomeTerrainValleys);
	TEST(testBiomeTerrainProfile);
	TEST(testBiomeTerrainFloaterSeeds);
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
class TestTerrainSampler final : public BiomeTerrainSampler {
public:
	explicit TestTerrainSampler(BiomeTerrain value) : value(value) {}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::make_unique<TestTerrainSampler>(value);
	}

	BiomeTerrain sample(v2s16 pos) const override
	{
		++calls;
		last_pos = pos;
		return value;
	}

	void resetCache() override { ++resets; }

	BiomeTerrain value;
	mutable unsigned int calls = 0;
	unsigned int resets = 0;
	mutable v2s16 last_pos;
};

class PlaneTerrainSampler final : public HeightmapBiomeTerrainSampler {
public:
	PlaneTerrainSampler(float dx, float dz, float offset) :
		m_dx(dx), m_dz(dz), m_offset(offset) {}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::make_unique<PlaneTerrainSampler>(m_dx, m_dz, m_offset);
	}

protected:
	float sampleHeight(s32 x, s32 z) const override
	{
		return m_dx * x + m_dz * z + m_offset;
	}

private:
	float m_dx;
	float m_dz;
	float m_offset;
};

class StepTerrainSampler final : public HeightmapBiomeTerrainSampler {
public:
	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::make_unique<StepTerrainSampler>();
	}

protected:
	float sampleHeight(s32 x, s32 z) const override
	{
		return x >= 0 ? 16.0f : 0.0f;
	}
};

class FoldTerrainSampler final : public HeightmapBiomeTerrainSampler {
public:
	enum Shape { RIDGE, VALLEY, TRIANGLE, DIAGONAL_PEAK };
	explicit FoldTerrainSampler(Shape shape) : m_shape(shape) {}

	std::unique_ptr<BiomeTerrainSampler> clone() const override
	{
		return std::make_unique<FoldTerrainSampler>(m_shape);
	}

protected:
	float sampleHeight(s32 x, s32 z) const override
	{
		switch (m_shape) {
		case RIDGE:
			return 100.0f - std::fabs(static_cast<float>(x));
		case VALLEY:
			return std::fabs(static_cast<float>(x));
		case TRIANGLE: {
			int phase = ((x % 16) + 16) % 16;
			return 2.0f * std::min(phase, 16 - phase);
		}
		case DIAGONAL_PEAK:
			return x == -8 && z == -8 ? 16.0f : 0.0f;
		}
		return 0.0f;
	}

private:
	Shape m_shape;
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

void assertTerrainEqual(const BiomeTerrain &actual, const BiomeTerrain &expected)
{
	UASSERTEQ(float, actual.height, expected.height);
	UASSERTEQ(float, actual.slope, expected.slope);
	UASSERTEQ(float, actual.relief, expected.relief);
}
}

void TestMapgen::testBiomeTerrainRanges()
{
	Biome biome;
	UASSERT(!biome.hasTerrainConstraints());
	UASSERTEQ(float, biome.slope_min, 0.0f);
	UASSERTEQ(float, biome.slope_max, 90.0f);
	UASSERTEQ(float, biome.relief_min, 0.0f);
	UASSERT(std::isinf(biome.relief_max) && biome.relief_max > 0.0f);
	UASSERT(biome.matchesTerrain({0.0f, 0.0f, 0.0f}));
	UASSERT(biome.matchesTerrain({1000.0f, 90.0f, 10000.0f}));

	// Exercise each optional bound by itself before combining the ranges.
	biome.slope_min = 10.0f;
	UASSERT(biome.hasTerrainConstraints());
	UASSERT(biome.matchesTerrain({0.0f, 10.0f, 0.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 9.99f, 0.0f}));
	biome.slope_min = 0.0f;
	biome.slope_max = 20.0f;
	UASSERT(biome.hasTerrainConstraints());
	UASSERT(biome.matchesTerrain({0.0f, 20.0f, 0.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 20.01f, 0.0f}));
	biome.slope_max = 90.0f;
	biome.relief_min = 30.0f;
	UASSERT(biome.hasTerrainConstraints());
	UASSERT(biome.matchesTerrain({0.0f, 0.0f, 30.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 0.0f, 29.99f}));
	biome.relief_min = 0.0f;
	biome.relief_max = 60.0f;
	UASSERT(biome.hasTerrainConstraints());
	UASSERT(biome.matchesTerrain({0.0f, 0.0f, 60.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 0.0f, 60.01f}));

	biome.slope_min = 10.0f;
	biome.slope_max = 20.0f;
	biome.relief_min = 30.0f;
	UASSERT(biome.matchesTerrain({-100.0f, 10.0f, 30.0f}));
	UASSERT(biome.matchesTerrain({100.0f, 20.0f, 60.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 15.0f, 100.0f}));
	UASSERT(!biome.matchesTerrain({0.0f, 30.0f, 40.0f}));
}

void TestMapgen::testBiomeTerrainParsing()
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
		UASSERT(parsed && !parsed->hasTerrainConstraints());
		UASSERT(std::isinf(parsed->relief_max));
	}
	lua_settop(L, 0);
	lua_newtable(L);
	number("slope_min", 10.0);
	number("slope_max", 20.0);
	number("relief_min", 30.0);
	number("relief_max", 60.0);
	{
		std::unique_ptr<Biome> parsed(read_biome_def(L, 1, ndef.get()));
		UASSERT(parsed && parsed->hasTerrainConstraints());
		UASSERTEQ(float, parsed->slope_min, 10.0f);
		UASSERTEQ(float, parsed->slope_max, 20.0f);
		UASSERTEQ(float, parsed->relief_min, 30.0f);
		UASSERTEQ(float, parsed->relief_max, 60.0f);
	}
	lua_settop(L, 0);

	for (const char *name : {"slope_min", "slope_max", "relief_min", "relief_max"}) {
		for (lua_Number invalid : {-1.0, std::numeric_limits<lua_Number>::infinity(),
				std::numeric_limits<lua_Number>::quiet_NaN(), 1.0e-300}) {
			lua_newtable(L);
			number(name, invalid);
			reject();
		}
		lua_newtable(L);
		lua_pushstring(L, "10");
		lua_setfield(L, -2, name);
		reject();
	}
	lua_newtable(L);
	number("slope_max", 90.0001);
	reject();
	lua_newtable(L);
	number("relief_max", static_cast<lua_Number>(std::numeric_limits<float>::max()) * 2.0);
	reject();

	// Comparing after conversion to float would silently accept these
	// inverted ranges because both endpoints round to the same value.
	for (const auto &names : {std::pair{"slope_min", "slope_max"},
			std::pair{"relief_min", "relief_max"}}) {
		lua_newtable(L);
		number(names.first, 10.00000001);
		number(names.second, 10.0);
		reject();
	}
}

void TestMapgen::testBiomeTerrainSelection(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(gamedef->getNodeDefManager());
	BiomeParamsOriginal params;
	params.seed = 12345;

	// No terrain constraints: preserve climate weighting and registration-order
	// tie breaking, and never pay for a terrain sample.
	addTerrainTestBiome(manager, "near", 40.0f, 40.0f);
	auto weighted = addTerrainTestBiome(manager, "weighted", 50.0f, 50.0f);
	weighted->weight = 8.0f;
	auto tied = addTerrainTestBiome(manager, "tied", 50.0f, 50.0f);
	tied->weight = 8.0f;
	BiomeGenOriginal original(&manager, &params, v3s16(16));
	UASSERT(original.calcBiomeFromNoise(30.0f, 30.0f, v3s16(0)) == weighted);
	auto provider = std::make_unique<TestTerrainSampler>(BiomeTerrain{50, 15, 40});
	auto *probe = provider.get();
	original.setTerrainSampler(std::move(provider));
	UASSERT(original.calcBiomeFromNoise(30.0f, 30.0f, v3s16(0)) == weighted);
	UASSERTEQ(unsigned int, probe->calls, 0);
	probe->value.height = std::numeric_limits<float>::quiet_NaN();
	UASSERT(original.calcBiomeFromNoise(30.0f, 30.0f, v3s16(0)) == weighted);
	UASSERTEQ(unsigned int, probe->calls, 0);

	MockBiomeManager gated_manager(&server);
	gated_manager.setNodeDefManager(gamedef->getNodeDefManager());
	auto flat = addTerrainTestBiome(gated_manager, "flat", 50.0f, 50.0f);
	flat->slope_max = 5.0f;
	auto hill = addTerrainTestBiome(gated_manager, "hill", 50.0f, 50.0f);
	hill->slope_min = 10.0f;
	hill->slope_max = 20.0f;
	hill->relief_min = 30.0f;
	hill->relief_max = 60.0f;
	auto fallback = addTerrainTestBiome(gated_manager, "fallback", 70.0f, 50.0f);
	fallback->min_pos.Y = 0;
	BiomeGenOriginal gated(&gated_manager, &params, v3s16(16));

	// A missing provider must not pretend the surface is flat.
	UASSERT(gated.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == fallback);
	UASSERTEQ(biome_t, gated.calcBiomeFromNoise(50.0f, 50.0f,
			v3s16(0, -1, 0))->index, BIOME_NONE);
	BiomeTerrain terrain;
	UASSERT(!gated.getBiomeTerrain(v2s16(0), terrain));

	provider = std::make_unique<TestTerrainSampler>(BiomeTerrain{50, 15, 40});
	probe = provider.get();
	gated.setTerrainSampler(std::move(provider));
	for (s16 y : {-100, 0, 100}) {
		UASSERT(gated.calcBiomeFromNoise(50.0f, 50.0f,
				v3s16(23, y, -17)) == hill);
		UASSERT(probe->last_pos == v2s16(23, -17));
	}
	UASSERT(probe->calls > 0);
	probe->value.slope = 3.0f;
	UASSERT(gated.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == flat);
	probe->value.slope = 15.0f;
	probe->value.relief = 100.0f;
	UASSERT(gated.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == fallback);
	probe->value.slope = 30.0f;
	probe->value.relief = 40.0f;
	UASSERTEQ(biome_t, gated.calcBiomeFromNoise(50.0f, 50.0f,
			v3s16(0, -1, 0))->index, BIOME_NONE);
	probe->value.height = std::numeric_limits<float>::quiet_NaN();
	UASSERT(!gated.getBiomeTerrain(v2s16(0), terrain));
	UASSERT(gated.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == fallback);

	// Cached terrain depends on the world position alone, so a generator
	// keeps it from one mapchunk to the next instead of discarding it.
	gated.calcBiomeNoise(v3s16(-8, -8, -8));
	gated.calcBiomeNoise(v3s16(72, 8, -88));
	UASSERTEQ(unsigned int, probe->resets, 0);

	// All indexed and bulk entry points must use the same terrain filter.
	probe->value = {50, 15, 40};
	params.np_heat.offset = params.np_humidity.offset = 50.0f;
	params.np_heat.scale = params.np_humidity.scale = 0.0f;
	params.np_heat_blend.scale = params.np_humidity_blend.scale = 0.0f;
	BiomeGenOriginal bulk(&gated_manager, &params, v3s16(16));
	bulk.setTerrainSampler(probe->clone());
	const v3s16 origin(-8, -8, -8);
	bulk.calcBiomeNoise(origin);
	std::vector<s16> heights(16 * 16, 0);
	auto *biomes = bulk.getBiomes(heights.data(), origin);
	for (size_t i = 0; i < heights.size(); ++i)
		UASSERTEQ(biome_t, biomes[i], hill->index);
	UASSERT(bulk.getBiomeAtIndex(0, origin) == hill);
	UASSERT(bulk.getBiomeAtPoint(v3s16(-1, 0, -1)) == hill);
	UASSERT(bulk.calcBiomeAtPoint(v3s16(-1, 0, -1)) == hill);
}

void TestMapgen::testBiomeTerrainBlending(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	manager.setNodeDefManager(gamedef->getNodeDefManager());
	auto lower = addTerrainTestBiome(manager, "lower", 50.0f, 50.0f);
	lower->max_pos.Y = 0;
	lower->vertical_blend = 8;
	lower->slope_max = 10.0f;
	auto upper = addTerrainTestBiome(manager, "upper", 90.0f, 50.0f);
	upper->min_pos.Y = 1;
	BiomeParamsOriginal params;
	params.seed = 0;
	BiomeGenOriginal generator(&manager, &params, v3s16(16));
	auto provider = std::make_unique<TestTerrainSampler>(BiomeTerrain{0, 30, 40});
	auto *probe = provider.get();
	generator.setTerrainSampler(std::move(provider));

	for (s16 y = 1; y <= 8; ++y)
		UASSERT(generator.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0, y, 0)) == upper);
	UASSERTEQ(biome_t, generator.calcBiomeFromNoise(50.0f, 50.0f,
			v3s16(0))->index, BIOME_NONE);

	probe->value.slope = 5.0f;
	UASSERT(generator.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == lower);
	unsigned int blended = 0;
	for (s16 y = 1; y <= 8; ++y) {
		for (int heat = 45; heat <= 55; ++heat) {
			auto biome = generator.calcBiomeFromNoise(heat, 50.0f, v3s16(0, y, 0));
			UASSERT(biome == lower || biome == upper);
			blended += biome == lower;
		}
	}
	UASSERT(blended > 0);
	UASSERT(generator.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0, 9, 0)) == upper);
}

void TestMapgen::testBiomeTerrainClone(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	MockBiomeManager manager(&server);
	auto biome = addTerrainTestBiome(manager, "bounded", 50.0f, 50.0f);
	biome->slope_min = 8.0f;
	biome->slope_max = 20.0f;
	biome->relief_min = 30.0f;
	biome->relief_max = 60.0f;
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
	UASSERTEQ(float, copied_biome->slope_min, 8.0f);
	UASSERTEQ(float, copied_biome->slope_max, 20.0f);
	UASSERTEQ(float, copied_biome->relief_min, 30.0f);
	UASSERTEQ(float, copied_biome->relief_max, 60.0f);

	BiomeParamsOriginal params;
	params.seed = 54321;
	BiomeGenOriginal original(&manager, &params, v3s16(16));
	auto provider = std::make_unique<TestTerrainSampler>(BiomeTerrain{100, 10, 40});
	auto *probe = provider.get();
	original.setTerrainSampler(std::move(provider));
	std::unique_ptr<BiomeGen> clone_base(original.clone(copied_manager.get()));
	auto *cloned = static_cast<BiomeGenOriginal *>(clone_base.get());
	UASSERT(original.calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == biome);
	UASSERT(cloned->calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == copied_biome);

	// Reconfiguring one provider must not affect the cloned worker's provider.
	probe->value.slope = 40.0f;
	UASSERTEQ(biome_t, original.calcBiomeFromNoise(50.0f, 50.0f,
			v3s16(0))->index, BIOME_NONE);
	UASSERT(cloned->calcBiomeFromNoise(50.0f, 50.0f, v3s16(0)) == copied_biome);
	BiomeTerrain terrain;
	UASSERT(cloned->getBiomeTerrain(v2s16(-31, 15), terrain));
	assertTerrainEqual(terrain, {100, 10, 40});
}

void TestMapgen::testBiomeTerrainMetrics()
{
	PlaneTerrainSampler flat(0.0f, 0.0f, 19.0f);
	assertTerrainEqual(flat.sample(v2s16(-17, 31)), {19, 0, 0});

	// An analytic plane also tests interpolation between lattice points and
	// sampling beyond the s16 boundary without wrapping the stencil.
	PlaneTerrainSampler plane(3.0f, 4.0f, 7.0f);
	const float slope = std::atan(5.0f) * 180.0f / std::acos(-1.0f);
	const v2s16 positions[] = {
		v2s16(0, 0), v2s16(-1, -1), v2s16(-17, 31), v2s16(15, -9),
		v2s16(S16_MIN, S16_MAX), v2s16(S16_MAX, S16_MAX)
	};
	for (auto pos : positions) {
		auto terrain = plane.sample(pos);
		UASSERTEQ(float, terrain.height, 3.0f * pos.X + 4.0f * pos.Y + 7.0f);
		UASSERT(std::fabs(terrain.slope - slope) < 0.0001f);
		UASSERTEQ(float, terrain.relief, 224.0f);
	}
	const auto expected = plane.sample(v2s16(-17, 31));
	plane.resetCache();
	plane.sample(v2s16(16, -32));
	assertTerrainEqual(plane.sample(v2s16(-17, 31)), expected);
	auto clone = plane.clone();
	clone->sample(v2s16(-100, -100));
	assertTerrainEqual(clone->sample(v2s16(-17, 31)), expected);

	// A voxel step becomes a gradual transition on the sampling lattice.
	// Its height and relief must not acquire a seam at the negative tile edge.
	StepTerrainSampler step;
	auto transition = step.sample(v2s16(-4, 13));
	UASSERTEQ(float, transition.height, 8.0f);
	const float step_slope = std::atan(2.0f) * 180.0f / std::acos(-1.0f);
	UASSERT(std::fabs(transition.slope - step_slope) < 0.0001f);
	UASSERTEQ(float, transition.relief, 16.0f);
	assertTerrainEqual(step.sample(v2s16(-24, 13)), {0, 0, 0});
	assertTerrainEqual(step.sample(v2s16(24, 13)), {16, 0, 0});
}

void TestMapgen::testBiomeTerrainFolds()
{
	Biome flat_biome;
	flat_biome.slope_max = 12.0f;
	flat_biome.relief_max = 20.0f;
	for (auto shape : {FoldTerrainSampler::RIDGE, FoldTerrainSampler::VALLEY}) {
		FoldTerrainSampler sampler(shape);
		for (s16 x : {-8, -4, -1, 0, 1, 2, 4, 8}) {
			auto terrain = sampler.sample(v2s16(x, -1));
			UASSERT(std::fabs(terrain.slope - 45.0f) < 0.0001f);
			UASSERT(!flat_biome.matchesTerrain(terrain));
		}
	}

	// Opposite slopes on each side of every lattice point must not cancel.
	// The relief of this rough surface is only 16, below the flat-biome cap.
	FoldTerrainSampler triangle(FoldTerrainSampler::TRIANGLE);
	const float flank = std::atan(2.0f) * 180.0f / std::acos(-1.0f);
	for (s16 x = -16; x <= 16; ++x) {
		auto terrain = triangle.sample(v2s16(x, 3));
		UASSERT(std::fabs(terrain.slope - flank) < 0.0001f);
		UASSERTEQ(float, terrain.relief, 16.0f);
		UASSERT(!flat_biome.matchesTerrain(terrain));
	}

	// Looking only along X/Z misses an isolated diagonal rise entirely.
	FoldTerrainSampler diagonal(FoldTerrainSampler::DIAGONAL_PEAK);
	auto terrain = diagonal.sample(v2s16(0));
	const float diagonal_flank = std::atan(std::sqrt(2.0f)) * 180.0f / std::acos(-1.0f);
	UASSERT(terrain.slope >= diagonal_flank - 0.0001f);
	UASSERTEQ(float, terrain.relief, 16.0f);
	UASSERT(!flat_biome.matchesTerrain(terrain));
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
	assertTerrainEqual(flat->sample(v2s16(-17, 31)), {19, 0, 0});

	// Inactive cliff carving must not convert an enormous but finite
	// surface directly to an integer before applying the generation bounds.
	params.spflags = MGVALLEYS_CARVE_CLIFFS;
	params.np_terrain_height.offset = 1.0e20f;
	params.np_rivers.offset = 2.0f;
	auto beyond_world = createValleysBiomeTerrainSampler(params);
	assertTerrainEqual(beyond_world->sample(v2s16(-17, 31)),
			{MAX_MAP_GENERATION_LIMIT, 0, 0});
	params.np_terrain_height.offset = 20.0f;
	params.np_rivers.offset = 1.0f;

	params.spflags = MGVALLEYS_MOUNTAINS;
	params.np_mountain_height.offset = 40.0f;
	params.np_mountain_height.scale = 0.0f;
	params.np_mountain.offset = 1.0f;
	params.np_mountain.scale = 0.0f;
	auto mountains = createValleysBiomeTerrainSampler(params);
	auto raised = mountains->sample(v2s16(-17, 31));
	UASSERT(raised.height > 29.0f);
	UASSERTEQ(float, raised.slope, 0.0f);
	UASSERTEQ(float, raised.relief, 0.0f);

	// The cap has to raise the modeled surface beyond the mountain body.
	// A general "mountains are higher" assertion would miss an omitted cap.
	params.np_mountain.offset = 0.1f;
	params.np_mountain_height.offset = 20.0f;
	params.mountain_cap = 0.0f;
	auto body = createValleysBiomeTerrainSampler(params);
	const auto body_surface = body->sample(v2s16(-17, 31));
	params.mountain_cap = 10.0f;
	auto capped = createValleysBiomeTerrainSampler(params);
	UASSERT(capped->sample(v2s16(-17, 31)).height > body_surface.height + 5.0f);
	params.spflags = 0;
	assertTerrainEqual(createValleysBiomeTerrainSampler(params)->sample(v2s16(-17, 31)),
			{19, 0, 0});
	params.spflags = MGVALLEYS_MOUNTAINS;
	for (float mask : {0.0f, -20.0f}) {
		params.np_mountain_height.offset = mask;
		assertTerrainEqual(createValleysBiomeTerrainSampler(params)->sample(v2s16(-17, 31)),
				{19, 0, 0});
	}
	params.np_mountain_height.offset = 20.0f;
	params.np_mountain.offset = -0.1f;
	assertTerrainEqual(createValleysBiomeTerrainSampler(params)->sample(v2s16(-17, 31)),
			{19, 0, 0});

	// At the centre of this constant river the bed is nine nodes below
	// the bank. Sea-level channels lower the bank from 20 to 2; their
	// bed must be allowed below the ordinary water_level - 3 clamp.
	params.spflags = 0;
	params.np_rivers.offset = 0.0f;
	params.river_depth = 8;
	auto elevated_river = createValleysBiomeTerrainSampler(params);
	assertTerrainEqual(elevated_river->sample(v2s16(-17, 31)), {10, 0, 0});
	params.spflags = MGVALLEYS_SEA_LEVEL_RIVERS;
	auto sea_river = createValleysBiomeTerrainSampler(params);
	assertTerrainEqual(sea_river->sample(v2s16(-17, 31)), {-8, 0, 0});
	params.floor_y = -6;
	auto floored_river = createValleysBiomeTerrainSampler(params);
	assertTerrainEqual(floored_river->sample(v2s16(-17, 31)), {-6, 0, 0});

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
	std::vector<BiomeTerrain> expected;
	for (auto pos : positions) {
		auto terrain = varied->sample(pos);
		UASSERT(std::isfinite(terrain.height));
		UASSERT(std::isfinite(terrain.slope) && terrain.slope >= 0 && terrain.slope <= 90);
		UASSERT(std::isfinite(terrain.relief) && terrain.relief >= 0);
		expected.push_back(terrain);
	}
	varied->resetCache();
	auto clone = varied->clone();
	for (size_t i = expected.size(); i-- > 0;) {
		assertTerrainEqual(varied->sample(positions[i]), expected[i]);
		assertTerrainEqual(clone->sample(positions[i]), expected[i]);
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

	// The central derivative at this fold was only 7.13 degrees despite
	// a six-node rise to an adjacent lattice point, eight nodes away.
	auto fold = raw->sample(v2s16(1344, -1872));
	const float adjacent_slope = std::atan(0.75f) * 180.0f / std::acos(-1.0f);
	UASSERT(fold.slope >= adjacent_slope - 0.0001f);
	Biome flat_biome;
	flat_biome.slope_max = 12.0f;
	flat_biome.relief_max = 20.0f;
	UASSERT(!flat_biome.matchesTerrain(fold));

	// The unconnected mountain cap at this column is removed by natural
	// floater cleanup. Its height must not be used to classify the ground.
	const v2s16 floater_pos(744, -1584);
	UASSERTEQ(float, raw->sample(floater_pos).height, 185.0f);
	params.spflags |= MGVALLEYS_REMOVE_FLOATERS;
	auto cleaned = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, cleaned->sample(floater_pos).height, 104.0f);

	// These heights come from native terrain generation with caves and
	// biome-material replacement disabled, before and after cliff carving.
	const v2s16 cliff_pos(680, -1584);
	UASSERTEQ(float, cleaned->sample(cliff_pos).height, 24.0f);
	params.spflags |= MGVALLEYS_CARVE_CLIFFS;
	auto carved = createValleysBiomeTerrainSampler(params);
	UASSERTEQ(float, carved->sample(cliff_pos).height, 13.0f);

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
		std::vector<BiomeTerrain> expected;
		for (auto pos : test.positions)
			expected.push_back(sampler->sample(pos));
		auto clone = sampler->clone();
		sampler->resetCache();
		for (size_t i = expected.size(); i-- > 0;) {
			UASSERT(std::isfinite(expected[i].height));
			UASSERT(std::isfinite(expected[i].slope));
			UASSERT(std::isfinite(expected[i].relief));
			assertTerrainEqual(sampler->sample(test.positions[i]), expected[i]);
			assertTerrainEqual(clone->sample(test.positions[i]), expected[i]);
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
		UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sample(
				column.pos).height, column.height);
		params.spflags = 0;
		UASSERT(createValleysBiomeTerrainSampler(params)->sample(
				column.pos).height > column.height);
	}

	// Every column top of these floating pieces has such a run, so the
	// native fill never starts on them and they stay: a lower top under a
	// column, or the top itself where nothing floats above it.
	const Column kept[] = {
		{216, v2s16(104, 0), 45}, {220, v2s16(80, 16), 44}, {309, v2s16(104, -8), 44}
	};
	for (const auto &column : kept) {
		auto params = floaterParams(column.seed);
		UASSERTEQ(float, createValleysBiomeTerrainSampler(params)->sample(
				column.pos).height, column.height);
		params.spflags = 0;
		UASSERT(createValleysBiomeTerrainSampler(params)->sample(
				column.pos).height >= column.height);
	}
}

#endif
