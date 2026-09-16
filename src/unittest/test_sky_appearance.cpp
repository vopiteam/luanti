// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#if IS_VOPI_ENGINE
#include "test.h"
#include "client/sky_appearance.h"

namespace {
void colorEqual(video::SColor actual, video::SColor expected)
{
	UASSERTEQ(u32, actual.color, expected.color);
}

void near(float actual, float expected, float tolerance = 0.000003f)
{
	UTEST(std::abs(actual - expected) <= tolerance,
			"%g != %g (tolerance %g)", actual, expected, tolerance);
}
}

class TestSkyAppearance : public TestBase {
public:
	TestSkyAppearance() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestSkyAppearance"; }
	void runTests(IGameDef *) override;
	void testFogColor();
	void testInitialExposureAndClock();
	void testCaveFrameRates();
	void testCaveReversal();
	void testFogFollowing();
	void testZeroFogDistance();
};
static TestSkyAppearance g_test_instance;

void TestSkyAppearance::runTests(IGameDef *)
{
	TEST(testFogColor);
	TEST(testInitialExposureAndClock);
	TEST(testCaveFrameRates);
	TEST(testCaveReversal);
	TEST(testFogFollowing);
	TEST(testZeroFogDistance);
}

void TestSkyAppearance::testFogColor()
{
	using SkyAppearance::fogColor;
	const video::SColor bg(255, 20, 40, 60);
	colorEqual( fogColor({0, 200, 100, 50}, bg, 1, true), bg);
	colorEqual( fogColor({255, 200, 100, 50}, bg, 1, true),
			video::SColor(255, 200, 100, 50));
	colorEqual( fogColor({128, 200, 100, 50}, bg, 1, true),
			video::SColor(255, 110, 70, 55));
	// Cave/night exposure dims the custom RGB before the alpha blend.
	colorEqual( fogColor({128, 200, 100, 50}, bg, 0.2f, true),
			video::SColor(255, 30, 30, 35));
	colorEqual( fogColor({255, 200, 100, 50}, bg, 0, true),
			video::SColor(255, 0, 0, 0));
	const video::SColor low_alpha(1, 200, 100, 50);
	// Disabled controlled appearance keeps the original absolute override contract.
	colorEqual( fogColor(low_alpha, bg, 0, false), low_alpha);
	colorEqual( fogColor({0, 200, 100, 50}, bg, 0, false), bg);
	colorEqual( fogColor(low_alpha, bg, 1, true),
			video::SColor(255, 21, 40, 60));
}

void TestSkyAppearance::testInitialExposureAndClock()
{
	SkyAppearance::Exposure exposure;
	exposure.update(true, 1, 1.0f / 60);
	near(exposure.brightness(1), 1, 0);
	// A clock jump at the surface is authoritative on the same frame.
	exposure.update(true, 0.1f, 1.0f / 60);
	near(exposure.brightness(0.1f), 0.1f, 0);
	const video::SColorf day(1, 0, 0), night(0, 0, 1), indoors(0, 1, 0);
	near(exposure.color(day, indoors).r, 1, 0);
	near(exposure.color(night, indoors).b, 1, 0);
	exposure.reset();
	exposure.update(false, 0.1f, 1.0f / 60);
	near(exposure.brightness(1), 0.1f, 0);
	near(exposure.brightness(0.2f), 0.1f, 0);
	near(exposure.color(day, indoors).g, 1, 0);
}

void TestSkyAppearance::testCaveFrameRates()
{
	for (int fps : {30, 60, 120}) {
		SkyAppearance::Exposure exposure;
		const float dt = 1.0f / fps;
		exposure.update(true, 1, dt);
		for (int i = 0; i < fps; ++i)
			exposure.update(false, 0.1f, dt);
		near(exposure.brightness(1), 0.1738765f);
		// Stabilize in the cave, then direct brightness jumps with sunlight.
		for (int i = 0; i < fps * 10; ++i)
			exposure.update(false, 0.1f, dt);
		exposure.update(true, 1, dt);
		UASSERT(exposure.brightness(1) > 0.1f);
		UASSERT(exposure.brightness(1) < 0.18f);
		for (int i = 1; i < fps; ++i)
			exposure.update(true, 1, dt);
		near(exposure.brightness(1), 0.9261235f);
		// Day/night palette changes during the exit use the current palette.
		const auto color = exposure.color({0, 0, 1}, {1, 0, 0});
		near(color.b, 0.917915f);
		near(color.r, 0.082085f);
	}
}

void TestSkyAppearance::testCaveReversal()
{
	SkyAppearance::Exposure exposure;
	exposure.update(false, 0.1f, 0);
	exposure.update(true, 1, 0.2f);
	const float before = exposure.brightness(1);
	near(before, 0.4541224f);
	// Re-entering a brighter cave must not replace a still-visible retained sample.
	exposure.update(false, 0.8f, 0);
	near(exposure.brightness(1), before, 0);
	exposure.update(false, 0.8f, 1.0f / 120);
	UASSERT(std::abs(exposure.brightness(1) - before) < 0.02f);
	for (int i = 0; i < 2400; ++i)
		exposure.update(false, 0.8f, 1.0f / 120);
	near(exposure.brightness(1), 0.8f);
}

void TestSkyAppearance::testFogFollowing()
{
	for (int fps : {30, 60, 120}) {
		SkyAppearance::Fog fog;
		const float dt = 1.0f / fps;
		fog.update(3000, 0.8f, 3000, dt, true);
		near(fog.range(), 3000, 0);
		near(fog.start(), 0.8f, 0);
		// An interval below float resolution must not snap to a distant target.
		fog.update(1000, 0.2f, 3000, 0.00000001f, true);
		UASSERT(fog.range() > 2999);
		for (int i = 0; i < fps; ++i)
			fog.update(1000, 0.2f, 3000, dt, true);
		near(fog.range(), 1013.4759f, 0.001f);
		near(fog.start(), 0.2040428f);
		// A lowered client limit is a hard cap, including in mid-transition.
		fog.update(400, 0.9f, 400, dt, true);
		near(fog.range(), 400, 0);
		fog.update(3000, 0.9f, 3000, dt, true);
		UASSERT(fog.range() > 400 && fog.range() < 1000);
		const float before = fog.range();
		fog.update(500, 0.1f, 3000, 0, true);
		near(fog.range(), before, 0);
		fog.update(100, 0.1f, 3000, dt, false);
		near(fog.range(), 100, 0);
		near(fog.start(), 0.1f, 0);
		fog.update(3000, 0.8f, 3000, dt, true);
		near(fog.range(), 3000, 0);
		for (int i = 0; i < fps * 10; ++i)
			fog.update(1234, 0.3f, 3000, dt, true);
		near(fog.range(), 1234, 0);
		near(fog.start(), 0.3f, 0);
	}
}

void TestSkyAppearance::testZeroFogDistance()
{
	for (float end : {0.0f, -1.0f, 0.000001f, 320000.0f}) {
		const float denominator = SkyAppearance::fogDistance(end);
		UASSERT(denominator > 0 && std::isfinite(denominator));
		if (end > 0)
			near(denominator, end, 0);
		else {
			// The shared fog shader expression must produce full fog, not 0 / 0.
			const float parameter = 0;
			near(parameter - parameter * 100 / denominator, 0, 0);
		}
	}
}
#endif
