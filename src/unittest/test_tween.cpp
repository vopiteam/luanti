// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#if IS_VOPI_ENGINE
#include "test.h"
#include "gui/tweenSpec.h"
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

using Result = TweenSpec::OptionResult;

void near(f32 actual, f32 expected, f32 tolerance = 1e-5f)
{
	UTEST(std::fabs(actual - expected) <= tolerance,
			"%.9g != %.9g (tolerance %.3g)", actual, expected, tolerance);
}

void nearPair(v2f actual, v2f expected, f32 tolerance = 1e-5f)
{
	near(actual.X, expected.X, tolerance);
	near(actual.Y, expected.Y, tolerance);
}

// Applies every option, which must all be accepted
TweenSpec makeSpec(std::initializer_list<std::pair<const char *, const char *>> options)
{
	TweenSpec spec;
	for (const auto &option : options) {
		std::string error;
		UTEST(spec.setOption(option.first, option.second, error) == Result::Ok,
				"option %s=%s refused: %s", option.first, option.second, error.c_str());
	}
	return spec;
}

Result apply(TweenSpec &spec, const char *name, const char *value)
{
	std::string error;
	return spec.setOption(name, value, error);
}

}

class TestTween : public TestBase {
public:
	TestTween() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestTween"; }
	void runTests(IGameDef *gamedef) override;
	void testDefaults();
	void testKeyFields();
	void testHoldAndInterpolate();
	void testEasings();
	void testCurve();
	void testLoop();
	void testFrames();
	void testInvalid();
	void testClamps();
	void testBoundaries();
	void testFramesWithTimeline();
	void testEasedValues();
};
static TestTween g_test_instance;

void TestTween::runTests(IGameDef *gamedef)
{
	TEST(testDefaults);
	TEST(testKeyFields);
	TEST(testHoldAndInterpolate);
	TEST(testEasings);
	TEST(testCurve);
	TEST(testLoop);
	TEST(testFrames);
	TEST(testInvalid);
	TEST(testClamps);
	TEST(testBoundaries);
	TEST(testFramesWithTimeline);
	TEST(testEasedValues);
}

void TestTween::testDefaults()
{
	TweenSpec spec;
	for (u64 t : {0ULL, 1000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
		TweenState s = spec.evaluate(t);
		nearPair(s.offset, v2f(0, 0));
		nearPair(s.scale, v2f(1, 1));
		near(s.alpha, 1.0f);
		UASSERTEQ(u32, s.frame, 0);
	}
}

void TestTween::testKeyFields()
{
	// The first key's empty fields take the defaults, later keys' empty
	// fields take the previous key's values
	TweenSpec spec = makeSpec({
		{"key", "0"},
		{"key", "100:1.5,-2:0.5,2:0.25"},
		{"key", "200::0.75"},
		{"key", "300:3,4"},
		{"key", " 400 : : : 0 : out : 1,1 "},
	});
	UASSERTEQ(size_t, spec.keys.size(), 5);

	const TweenKey &k0 = spec.keys[0];
	UASSERTEQ(u32, k0.time_ms, 0);
	nearPair(k0.offset, v2f(0, 0));
	nearPair(k0.scale, v2f(1, 1));
	near(k0.alpha, 1.0f);

	const TweenKey &k1 = spec.keys[1];
	nearPair(k1.offset, v2f(1.5f, -2.0f));
	nearPair(k1.scale, v2f(0.5f, 2.0f));
	near(k1.alpha, 0.25f);

	// A single scale value scales both axes
	const TweenKey &k2 = spec.keys[2];
	nearPair(k2.offset, v2f(1.5f, -2.0f));
	nearPair(k2.scale, v2f(0.75f, 0.75f));
	near(k2.alpha, 0.25f);

	const TweenKey &k3 = spec.keys[3];
	nearPair(k3.offset, v2f(3, 4));
	nearPair(k3.scale, v2f(0.75f, 0.75f));

	// Easing and control point belong to one segment and are not inherited
	const TweenKey &k4 = spec.keys[4];
	UASSERTEQ(u32, k4.time_ms, 400);
	nearPair(k4.offset, v2f(3, 4));
	near(k4.alpha, 0.0f);
	UASSERT(k4.easing == TweenEasing::Out);
	UASSERT(k4.curved);
	nearPair(k4.control, v2f(1, 1));
	UASSERT(spec.keys[3].easing == TweenEasing::Linear);
	UASSERT(!spec.keys[3].curved);
}

void TestTween::testHoldAndInterpolate()
{
	TweenSpec spec = makeSpec({
		{"key", "200:0,0:1:1"},
		{"key", "600:4,-2:0.5:0"},
	});

	// Before the first key: the first key holds
	nearPair(spec.evaluate(0).offset, v2f(0, 0));
	near(spec.evaluate(199).alpha, 1.0f);
	// Midway: linear by default
	TweenState mid = spec.evaluate(400);
	nearPair(mid.offset, v2f(2, -1));
	nearPair(mid.scale, v2f(0.75f, 0.75f));
	near(mid.alpha, 0.5f);
	// After the last key: the last key holds
	TweenState end = spec.evaluate(5000);
	nearPair(end.offset, v2f(4, -2));
	near(end.alpha, 0.0f);

	// Two keys at one time jump from one value to the other
	TweenSpec jump = makeSpec({
		{"key", "0:0,0"},
		{"key", "100:1,0"},
		{"key", "100:5,0"},
		{"key", "200:6,0"},
	});
	nearPair(jump.evaluate(99).offset, v2f(0.99f, 0), 1e-4f);
	nearPair(jump.evaluate(100).offset, v2f(5, 0));
	nearPair(jump.evaluate(150).offset, v2f(5.5f, 0));
}

void TestTween::testEasings()
{
	auto at_half = [](const char *easing, u64 t = 50) {
		TweenSpec spec;
		std::string error;
		spec.setOption("key", "0:0,0", error);
		spec.setOption("key", std::string("100:1,0:::") + easing, error);
		return spec.evaluate(t).offset.X;
	};
	near(at_half("linear"), 0.5f);
	near(at_half("in"), 0.125f);
	near(at_half("out"), 0.875f);
	near(at_half("in_out"), 0.5f);
	near(at_half("in_out", 25), 0.0625f);
	near(at_half("in_out", 75), 0.9375f);
	// out_back passes the end, then settles on it
	UASSERT(at_half("out_back", 70) > 1.0f);
	near(at_half("out_back", 0), 0.0f);
	near(at_half("out_back", 100), 1.0f);
}

void TestTween::testCurve()
{
	TweenSpec spec = makeSpec({
		{"key", "0:0,0"},
		{"key", "100:2,0:::linear:1,2"},
	});
	// Quadratic Bezier: B(0.5) = P0/4 + C/2 + P1/4
	nearPair(spec.evaluate(50).offset, v2f(1, 1));
	nearPair(spec.evaluate(0).offset, v2f(0, 0));
	nearPair(spec.evaluate(100).offset, v2f(2, 0));
	nearPair(spec.evaluate(25).offset, v2f(0.5f, 0.75f));
}

void TestTween::testLoop()
{
	TweenSpec spec = makeSpec({
		{"key", "0:0,0:1"},
		{"key", "300::0.8"},
		{"key", "600::1"},
		{"loop", "true"},
	});
	near(spec.evaluate(300).scale.X, 0.8f);
	near(spec.evaluate(900).scale.X, 0.8f);
	near(spec.evaluate(600).scale.X, 1.0f);
	near(spec.evaluate(150 + 600 * 1000).scale.X, 0.9f);
	// A huge elapsed time does not overflow
	TweenState s = spec.evaluate(0xFFFFFFFFFFFFFFFFULL);
	UASSERT(std::isfinite(s.scale.X));

	// Without loop the last key holds
	TweenSpec once = makeSpec({{"key", "0::1"}, {"key", "600::0.5"}});
	near(once.evaluate(900).scale.X, 0.5f);

	// A timeline of zero length cannot loop and holds its keys
	TweenSpec zero = makeSpec({{"key", "0::0.5"}, {"loop", "yes"}});
	near(zero.evaluate(1234).scale.X, 0.5f);
}

void TestTween::testFrames()
{
	TweenSpec spec = makeSpec({{"frames", "4"}, {"columns", "2"}, {"frame_ms", "100"}});
	UASSERTEQ(u32, spec.evaluate(0).frame, 0);
	UASSERTEQ(u32, spec.evaluate(99).frame, 0);
	UASSERTEQ(u32, spec.evaluate(100).frame, 1);
	UASSERTEQ(u32, spec.evaluate(399).frame, 3);
	UASSERTEQ(u32, spec.evaluate(400).frame, 0);

	UASSERT(apply(spec, "frame_loop", "false") == Result::Ok);
	UASSERTEQ(u32, spec.evaluate(400).frame, 3);
	UASSERTEQ(u32, spec.evaluate(0xFFFFFFFFFFFFFFFFULL).frame, 3);

	// frame_ms 0 shows the first frame
	TweenSpec still = makeSpec({{"frames", "4"}});
	UASSERTEQ(u32, still.evaluate(1000).frame, 0);
}

void TestTween::testInvalid()
{
	TweenSpec spec;
	UASSERT(apply(spec, "texture", "a.png") == Result::Unknown);
	UASSERT(apply(spec, "frames", "0") == Result::Invalid);
	UASSERT(apply(spec, "frames", "1025") == Result::Invalid);
	UASSERT(apply(spec, "frames", "-1") == Result::Invalid);
	UASSERT(apply(spec, "frames", "2x") == Result::Invalid);
	UASSERT(apply(spec, "columns", "") == Result::Invalid);
	UASSERT(apply(spec, "frame_ms", "4294967296") == Result::Invalid);
	UASSERT(apply(spec, "loop", "maybe") == Result::Invalid);
	UASSERTEQ(u32, spec.frames, 1);

	UASSERT(apply(spec, "key", "") == Result::Invalid);
	UASSERT(apply(spec, "key", "-5") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:nan,0") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:inf,0") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:1e40,0") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:1") == Result::Invalid);
	UASSERT(apply(spec, "key", "0::1,2,3") == Result::Invalid);
	UASSERT(apply(spec, "key", "0::::bounce") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:::::1") == Result::Invalid);
	UASSERT(apply(spec, "key", "0:0,0:1:1:linear:0,0:x") == Result::Invalid);
	UASSERTEQ(size_t, spec.keys.size(), 0);

	UASSERT(apply(spec, "key", "500") == Result::Ok);
	UASSERT(apply(spec, "key", "499") == Result::Invalid);
	UASSERTEQ(size_t, spec.keys.size(), 1);

	TweenSpec full;
	for (size_t i = 0; i < TweenSpec::MAX_KEYS; i++)
		UASSERT(apply(full, "key", std::to_string(i).c_str()) == Result::Ok);
	UASSERT(apply(full, "key", "1000") == Result::Invalid);
	UASSERTEQ(size_t, full.keys.size(), TweenSpec::MAX_KEYS);
}

void TestTween::testClamps()
{
	TweenSpec spec = makeSpec({
		{"key", "0:1e9,-1e9:-1,5000:2"},
		{"key", "100:0,0:1:-1:linear:1e9,0"},
	});
	nearPair(spec.keys[0].offset, v2f(TweenSpec::MAX_OFFSET, -TweenSpec::MAX_OFFSET));
	nearPair(spec.keys[0].scale, v2f(0, TweenSpec::MAX_SCALE));
	near(spec.keys[0].alpha, 1.0f);
	near(spec.keys[1].alpha, 0.0f);
	nearPair(spec.keys[1].control, v2f(TweenSpec::MAX_OFFSET, 0));

	// An overshooting easing never gives a negative scale or alpha out of range
	TweenSpec back = makeSpec({
		{"key", "0::1:1"},
		{"key", "100::0:0:out_back"},
	});
	for (u64 t = 0; t <= 100; t += 5) {
		TweenState s = back.evaluate(t);
		UASSERT(s.scale.X >= 0.0f && s.scale.Y >= 0.0f);
		UASSERT(s.alpha >= 0.0f && s.alpha <= 1.0f);
	}
}

void TestTween::testBoundaries()
{
	// The largest accepted values
	TweenSpec spec = makeSpec({
		{"frames", "1024"},
		{"columns", "1024"},
		{"frame_ms", "4294967295"},
		{"key", "4294967295"},
	});
	UASSERTEQ(u32, spec.frames, 1024);
	UASSERTEQ(u32, spec.frame_ms, 4294967295U);
	UASSERTEQ(u32, spec.keys[0].time_ms, 4294967295U);

	// Every spelling of a boolean
	TweenSpec flags;
	UASSERT(apply(flags, "loop", "1") == Result::Ok && flags.loop);
	UASSERT(apply(flags, "loop", "no") == Result::Ok && !flags.loop);
	UASSERT(apply(flags, "loop", " YES ") == Result::Ok && flags.loop);
	UASSERT(apply(flags, "loop", "0") == Result::Ok && !flags.loop);
}

void TestTween::testFramesWithTimeline()
{
	// The frames follow the time since the start, not the looping timeline
	TweenSpec spec = makeSpec({
		{"frames", "4"},
		{"frame_ms", "100"},
		{"key", "0:0,0"},
		{"key", "300:3,0"},
		{"loop", "true"},
	});
	TweenState s = spec.evaluate(350);
	UASSERTEQ(u32, s.frame, 3);
	nearPair(s.offset, v2f(0.5f, 0));

	// A single frame never changes
	TweenSpec single = makeSpec({{"frames", "1"}, {"frame_ms", "100"}});
	UASSERTEQ(u32, single.evaluate(550).frame, 0);

	// In a loop the first key holds before its time in every period
	TweenSpec delayed = makeSpec({
		{"key", "200:0,0"},
		{"key", "600:4,0"},
		{"loop", "true"},
	});
	nearPair(delayed.evaluate(700).offset, v2f(0, 0));
	nearPair(delayed.evaluate(1000).offset, v2f(2, 0));
}

void TestTween::testEasedValues()
{
	// The easing shapes scale and alpha as it shapes the offset
	TweenSpec spec = makeSpec({
		{"key", "0::1:1"},
		{"key", "100::0:0:in"},
	});
	TweenState s = spec.evaluate(50);
	nearPair(s.scale, v2f(0.875f, 0.875f));
	near(s.alpha, 0.875f);

	// and the position along a curve: B(e) with e = 0.5^3
	TweenSpec curve = makeSpec({
		{"key", "0:0,0"},
		{"key", "100:2,0:::in:1,2"},
	});
	nearPair(curve.evaluate(50).offset, v2f(0.25f, 0.4375f));
}
#endif
