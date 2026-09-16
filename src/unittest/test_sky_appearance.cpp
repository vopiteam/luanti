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
	void testPaletteFollowing();
	void testPaletteFogHue();
	void testPaletteSnapWindow();
	void testPaletteLandsWhole();
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
	TEST(testPaletteFollowing);
	TEST(testPaletteFogHue);
	TEST(testPaletteSnapWindow);
	TEST(testPaletteLandsWhole);
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

void TestSkyAppearance::testPaletteFollowing()
{
	using SkyAppearance::Palette;
	const video::SColor red(255, 255, 0, 0), blue(255, 0, 0, 255);
	const video::SColor white(255, 255, 255, 255), grey(255, 128, 128, 128);
	const SkyColor first = {red, red, red, red, red, red, red};
	const SkyColor second = {blue, blue, blue, blue, blue, blue, blue};
	const video::SColor no_fog(0, 200, 100, 50), full_fog(255, 200, 100, 50);
	for (int fps : {30, 60, 120}) {
		Palette palette;
		const float dt = 1.0f / fps;
		UASSERT(!palette.initialized());
		// The first sample shows at once, whatever the frame time.
		palette.update(first, no_fog, white, white, 0);
		UASSERT(palette.initialized());
		near(palette.color(Palette::DAY_SKY).r, 1, 0);
		near(palette.color(Palette::FOG).a, 0, 0);
		colorEqual(palette.fog(), no_fog);
		// One second towards the new target lands at e^-2 of the way back.
		for (int i = 0; i < fps; ++i)
			palette.update(second, full_fog, grey, grey, dt);
		near(palette.color(Palette::DAY_SKY).r, 0.1353353f, 0.0005f);
		near(palette.color(Palette::NIGHT_HORIZON).b, 0.8646647f, 0.0005f);
		near(palette.color(Palette::INDOORS).g, 0, 0);
		near(palette.color(Palette::SUN_TINT).r, 0.5693634f, 0.0005f);
		// Fog opacity eases too, the hue stays the fog's own.
		near(palette.color(Palette::FOG).a, 0.8646647f, 0.0005f);
		UASSERT(palette.fog().getAlpha() >= 219 && palette.fog().getAlpha() <= 221);
		UASSERTEQ(u32, palette.fog().getRed(), 200);
		// A frame without elapsed time changes nothing.
		const float held = palette.color(Palette::DAY_SKY).r;
		palette.update(first, no_fog, white, white, 0);
		near(palette.color(Palette::DAY_SKY).r, held, 0);
		// Ten seconds land exactly on the target, no visible tail.
		for (int i = 0; i < fps * 10; ++i)
			palette.update(second, full_fog, grey, grey, dt);
		near(palette.color(Palette::DAY_SKY).r, 0, 0);
		near(palette.color(Palette::DAWN_HORIZON).b, 1, 0);
		colorEqual(palette.fog(), full_fog);
		near(palette.color(Palette::MOON_TINT).g, grey.getGreen() / 255.0f, 0);
		// A reset shows the next sample at once again.
		palette.reset();
		palette.update(first, no_fog, white, white, dt);
		near(palette.color(Palette::DAY_SKY).r, 1, 0);
		colorEqual(palette.fog(), no_fog);
	}
}

void TestSkyAppearance::testPaletteFogHue()
{
	using SkyAppearance::Palette;
	const video::SColor red(255, 255, 0, 0), white(255, 255, 255, 255);
	const SkyColor sky = {red, red, red, red, red, red, red};
	const video::SColor rose(255, 200, 100, 50), clear_blue(0, 0, 0, 255);
	const video::SColor blue(255, 0, 0, 255);
	const float dt = 1.0f / 60;
	Palette palette;
	// The stock fog is transparent black. The first explicit fog takes its
	// own hue at once; only its opacity eases in.
	palette.update(sky, video::SColor(0), white, white, dt);
	palette.update(sky, rose, white, white, dt);
	UASSERTEQ(u32, palette.fog().getRed(), 200);
	UASSERTEQ(u32, palette.fog().getBlue(), 50);
	UASSERT(palette.fog().getAlpha() > 0 && palette.fog().getAlpha() < 255);
	for (int i = 0; i < 600; ++i)
		palette.update(sky, rose, white, white, dt);
	colorEqual(palette.fog(), rose);
	// Fading out keeps the hue: the transparent target's color never shows.
	for (int i = 0; i < 60; ++i)
		palette.update(sky, clear_blue, white, white, dt);
	UASSERTEQ(u32, palette.fog().getRed(), 200);
	UASSERTEQ(u32, palette.fog().getBlue(), 50);
	near(palette.color(Palette::FOG).a, 0.1353353f, 0.0005f);
	for (int i = 0; i < 600; ++i)
		palette.update(sky, clear_blue, white, white, dt);
	UASSERTEQ(u32, palette.fog().getAlpha(), 0);
	UASSERTEQ(u32, palette.fog().getRed(), 200);
	// From transparent, a new hue shows at once again.
	palette.update(sky, blue, white, white, dt);
	UASSERTEQ(u32, palette.fog().getRed(), 0);
	UASSERTEQ(u32, palette.fog().getBlue(), 255);
	UASSERT(palette.fog().getAlpha() > 0 && palette.fog().getAlpha() < 255);
}

void TestSkyAppearance::testPaletteSnapWindow()
{
	using SkyAppearance::Palette;
	const video::SColor red(255, 255, 0, 0), blue(255, 0, 0, 255), green(255, 0, 255, 0);
	const SkyColor first = {red, red, red, red, red, red, red};
	const SkyColor second = {blue, blue, blue, blue, blue, blue, blue};
	const SkyColor third = {green, green, green, green, green, green, green};
	const video::SColor white(255, 255, 255, 255), no_fog(0);
	const float dt = 1.0f / 60;
	Palette palette;
	// The window starts open: the first packet of a session shows at once
	// even when it arrives after the first frame showed the stock palette.
	palette.update(first, no_fog, white, white, dt);
	palette.packetArrived();
	palette.update(second, no_fog, white, white, dt);
	near(palette.color(Palette::DAY_SKY).b, 1, 0);
	// The next packet eases.
	palette.packetArrived();
	palette.update(third, no_fog, white, white, dt);
	UASSERT(palette.color(Palette::DAY_SKY).g < 0.5f);
	UASSERT(palette.color(Palette::DAY_SKY).b > 0.5f);
	for (int i = 0; i < 600; ++i)
		palette.update(third, no_fog, white, white, dt);
	near(palette.color(Palette::DAY_SKY).g, 1, 0);
	// A packet within a second of a teleport shows at once ...
	palette.expectPacket(1.0f);
	for (int i = 0; i < 30; ++i)
		palette.update(third, no_fog, white, white, dt);
	palette.packetArrived();
	palette.update(first, no_fog, white, white, dt);
	near(palette.color(Palette::DAY_SKY).r, 1, 0);
	// ... and one arriving later eases as usual.
	palette.expectPacket(1.0f);
	for (int i = 0; i < 90; ++i)
		palette.update(first, no_fog, white, white, dt);
	palette.packetArrived();
	palette.update(second, no_fog, white, white, dt);
	UASSERT(palette.color(Palette::DAY_SKY).r > 0.5f);
	// One window, one snap: a second packet in the same window eases.
	palette.expectPacket(1.0f);
	palette.packetArrived();
	palette.update(third, no_fog, white, white, dt);
	near(palette.color(Palette::DAY_SKY).g, 1, 0);
	palette.packetArrived();
	palette.update(first, no_fog, white, white, dt);
	UASSERT(palette.color(Palette::DAY_SKY).r < 0.5f);
}

void TestSkyAppearance::testPaletteLandsWhole()
{
	using SkyAppearance::Palette;
	const video::SColor from(255, 255, 0, 0), to(255, 0, 0, 8);
	const SkyColor start = {from, from, from, from, from, from, from};
	const SkyColor end = {to, to, to, to, to, to, to};
	const video::SColor white(255, 255, 255, 255), no_fog(0);
	const float dt = 1.0f / 60;
	const float target_blue = 8.0f / 255;
	Palette palette;
	palette.update(start, no_fog, white, white, dt);
	// After three seconds red is still e^-6 away, so the short blue travel,
	// far inside the landing distance, waits for it.
	for (int i = 0; i < 180; ++i)
		palette.update(end, no_fog, white, white, dt);
	UASSERT(palette.color(Palette::DAY_SKY).r > Palette::LANDING);
	UASSERT(palette.color(Palette::DAY_SKY).b != target_blue);
	UASSERT(std::abs(palette.color(Palette::DAY_SKY).b - target_blue) < Palette::LANDING);
	// A second later the whole color lands together.
	for (int i = 0; i < 60; ++i)
		palette.update(end, no_fog, white, white, dt);
	near(palette.color(Palette::DAY_SKY).r, 0, 0);
	near(palette.color(Palette::DAY_SKY).b, target_blue, 0);
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
