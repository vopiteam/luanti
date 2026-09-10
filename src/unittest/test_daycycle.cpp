// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#if IS_VOPI_ENGINE
#include "test.h"
#include "daycycle.h"
#include "daynightratio.h"
#include "environment.h"
#include "map.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
DayCycleDefinition exampleCycle()
{
	DayCycleDefinition d;
	d.enabled = true;
	d.day_start = d.sunrise = 5.0 / 24;
	d.night_start = d.sunset = 21.0 / 24;
	d.day_duration = 1080;
	d.night_duration = 360;
	d.dawn_start = 4.0 / 24;
	d.dawn_end = 6.0 / 24;
	d.dusk_start = 20.0 / 24;
	d.dusk_end = 22.0 / 24;
	d.validate();
	return d;
}

void near(double actual, double expected, double tolerance = 1e-9)
{
	UTEST(std::abs(actual - expected) <= tolerance,
			"%.17g != %.17g (tolerance %.3g)", actual, expected, tolerance);
}

class ClockEnvironment : public Environment {
public:
	ClockEnvironment(IGameDef *gamedef) : Environment(gamedef) {}
	void step(float dtime) override { stepTimeOfDay(dtime); }
	Map &getMap() override { throw std::logic_error("clock test does not access map"); }
	void getSelectedActiveObjects(const core::line3d<f32> &, std::vector<PointedThing> &,
			const std::optional<Pointabilities> &) override {}
};
}

class TestDayCycle : public TestBase {
public:
	TestDayCycle() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestDayCycle"; }
	void runTests(IGameDef *gamedef) override;
	void testDurations();
	void testPartition();
	void testFreezeAndCalendar(IGameDef *gamedef);
	void testVisuals();
	void testValidation();
	void testLegacy();
	void testSnapshots();
	void testMigration();
	void testClockText();
	void testClockMinuteBoundaries(IGameDef *gamedef);
};
static TestDayCycle g_test_instance;

void TestDayCycle::runTests(IGameDef *gamedef)
{
	TEST(testDurations);
	TEST(testPartition);
	TEST(testFreezeAndCalendar, gamedef);
	TEST(testVisuals);
	TEST(testValidation);
	TEST(testLegacy);
	TEST(testSnapshots);
	TEST(testMigration);
	TEST(testClockText);
	TEST(testClockMinuteBoundaries, gamedef);
}

void TestDayCycle::testDurations()
{
	auto d = exampleCycle();
	DayCycleTime time{7, d.day_start, false};
	time.advance(1080, d, 0);
	near(time.timeofday, d.night_start);
	UASSERTEQ(uint32_t, time.day, 7);
	time.advance(360, d, 0);
	near(time.timeofday, d.day_start);
	UASSERTEQ(uint32_t, time.day, 8);
	time.advance(1440 * 10000, d, 0);
	near(time.timeofday, d.day_start);
	UASSERTEQ(uint32_t, time.day, 10008);
	near(d.speed(0.5), 86400.0 * (16.0 / 24) / 1080);
	near(d.speed(0), 80);
}

void TestDayCycle::testPartition()
{
	auto d = exampleCycle();
	for (double start : {0.0, 0.1, 5.0 / 24, 0.5, 21.0 / 24, 0.999}) {
		DayCycleTime whole{12, start, false}, parts = whole;
		double elapsed = 0;
		for (unsigned i = 0; i < 20000; ++i) {
			double dt = (i % 13 + 1) * 0.037;
			parts.advance(dt, d, 0);
			elapsed += dt;
		}
		whole.advance(elapsed, d, 0);
		UASSERTEQ(uint32_t, parts.day, whole.day);
		near(parts.timeofday, whole.timeofday, 1e-8);
	}
	// A legal profile whose daytime and visual dawn cross civil midnight.
	d.day_start = 0.8;
	d.night_start = 0.3;
	DayCycleTime time{0, 0.9, false};
	time.advance(d.day_duration + d.night_duration, d, 0);
	near(time.timeofday, 0.9);
	UASSERTEQ(uint32_t, time.day, 1);
}

void TestDayCycle::testFreezeAndCalendar(IGameDef *gamedef)
{
	ClockEnvironment env(gamedef);
	env.setWorldTime({10, 0.5, false});
	env.setTimeOfDaySpeed(0);
	for (int i = 0; i < 120; ++i)
		env.stepTimeOfDay(0.5);
	env.setTimeOfDaySpeed(60);
	env.stepTimeOfDay(0.1);
	UASSERTEQ(u32, env.getTimeOfDay(), 12001);
	near(env.getTimeOfDayF(), 0.5 + 0.1 * 60 / 86400, 1e-10);
	UASSERTEQ(u32, env.getDayCount(), 10);
	env.setDayCycle(exampleCycle());
	env.setDayCyclePaused(true);
	double fixed = env.getTimeOfDayF();
	for (int i = 0; i < 120; ++i)
		env.stepTimeOfDay(0.5);
	near(env.getTimeOfDayF(), fixed, 0);
	UASSERTEQ(u32, env.getDayCount(), 10);
	env.setDayCyclePaused(false);
	env.stepTimeOfDay(1440);
	near(env.getTimeOfDayF(), fixed);
	UASSERTEQ(u32, env.getDayCount(), 11);
	// Snapshot correction, unlike the legacy setter, must not infer a new day.
	env.setWorldTime({11, 0.1, false});
	UASSERTEQ(u32, env.getDayCount(), 11);
	env.advanceTime(86400 * 3 + 43200);
	UASSERTEQ(u32, env.getDayCount(), 14);
	near(env.getTimeOfDayF(), 0.6);
	env.setTimeOfDay(2400);
	UASSERTEQ(u32, env.getDayCount(), 15);
}

void TestDayCycle::testVisuals()
{
	auto d = exampleCycle();
	near(evaluateDayCycle(d, d.sunrise).orbit_time, 0.25);
	near(evaluateDayCycle(d, 13.0 / 24).orbit_time, 0.5);
	near(evaluateDayCycle(d, d.sunset).orbit_time, 0.75);
	near(evaluateDayCycle(d, d.sunrise).shadow_factor, 0);
	near(evaluateDayCycle(d, d.sunset).shadow_factor, 0);
	for (int i = 0; i < 86400; ++i) {
		auto s = evaluateDayCycle(d, i / 86400.0);
		near(s.day_weight + s.dawn_weight + s.night_weight, 1);
		UASSERT(s.day_weight >= 0 && s.dawn_weight >= 0 && s.night_weight >= 0);
		UASSERT(s.daylight >= 0 && s.daylight <= 1);
		UASSERT(s.day_night_ratio >= d.night_light && s.day_night_ratio <= d.day_light);
	}
	for (double t : {d.dawn_start, d.sunrise, d.dawn_end, d.dusk_start, d.sunset, d.dusk_end}) {
		auto a = evaluateDayCycle(d, t - 1e-10);
		auto b = evaluateDayCycle(d, t + 1e-10);
		near(a.day_night_ratio, b.day_night_ratio, 1e-7);
		near(a.dawn_weight, b.dawn_weight, 1e-7);
	}
}

void TestDayCycle::testValidation()
{
	auto d = exampleCycle();
	UASSERTEQ(std::string, DayCycleDefinition::deserialize(d.serialize()).serialize(), d.serialize());
	EXCEPTION_CHECK(std::invalid_argument, DayCycleDefinition::deserialize(d.serialize() + " trailing"));
	EXCEPTION_CHECK(std::invalid_argument, DayCycleDefinition::deserialize("2 1"));
	d.day_duration = std::numeric_limits<double>::quiet_NaN();
	EXCEPTION_CHECK(std::invalid_argument, d.validate());
	d = exampleCycle();
	d.dawn_end = d.dawn_start;
	EXCEPTION_CHECK(std::invalid_argument, d.validate());
	DayCycleTime time{UINT32_MAX, 0.75, false};
	EXCEPTION_CHECK(std::invalid_argument, time.advanceDays(0.5));
	near(time.timeofday, 0.75);
	UASSERTEQ(uint32_t, time.day, UINT32_MAX);
	EXCEPTION_CHECK(std::invalid_argument, time.advanceDays(-1));
}

void TestDayCycle::testLegacy()
{
	DayCycleDefinition d;
	for (int i = 0; i < 24000; ++i) {
		double time = i / 24000.0;
		near(evaluateDayCycle(d, time).day_night_ratio,
				time_to_daynight_ratio(time * 24000, true) / 1000.0, 0);
	}
	DayCycleTime time{2, 0.25, false};
	time.advance(86400 * 3, d, 1);
	near(time.timeofday, 0.25);
	UASSERTEQ(uint32_t, time.day, 5);
}

void TestDayCycle::testSnapshots()
{
	DayCycleSnapshot s{exampleCycle(), {123, 0.123456789123, true}, 8, 19};
	auto decoded = DayCycleSnapshot::deserialize(s.serialize());
	UASSERTEQ(std::string, decoded.serialize(), s.serialize());
	EXCEPTION_CHECK(std::invalid_argument, DayCycleSnapshot::deserialize(s.serialize() + " trailing"));
	EXCEPTION_CHECK(std::invalid_argument, DayCycleSnapshot::deserialize("2 1 0 0 0 0 \"1 0\""));
	s.clock.timeofday = 1;
	EXCEPTION_CHECK(std::invalid_argument, DayCycleSnapshot::deserialize(s.serialize()));
	s.clock.timeofday = std::numeric_limits<double>::infinity();
	EXCEPTION_CHECK(std::invalid_argument, DayCycleSnapshot::deserialize(s.serialize()));
}

void TestDayCycle::testMigration()
{
	auto d = exampleCycle();
	for (auto example : {std::pair<double, double>{0, 1}, {5, 5}, {12, 13},
			{19, 21}, {22.75, 24}, {23, 24.2}}) {
		auto migrated = migrateDayCycleTime({7, example.first / 24, true},
				5.0 / 24, 19.0 / 24, d);
		UASSERTEQ(uint32_t, migrated.day, example.second >= 24 ? 8 : 7);
		near(migrated.timeofday, dayCycleWrap(example.second / 24));
		UASSERT(migrated.paused);
	}
	DayCycleTime time{7, d.night_start, false};
	time.advance(135, d, 0);
	UASSERTEQ(uint32_t, time.day, 8);
	near(time.timeofday, 0);
}

void TestDayCycle::testClockText()
{
	UASSERTEQ(std::string, formatDayCycleTime(0, false), "00:00");
	UASSERTEQ(std::string, formatDayCycleTime(0.5, false), "12:00");
	UASSERTEQ(std::string, formatDayCycleTime(13.0 / 24, false), "13:00");
	UASSERTEQ(std::string, formatDayCycleTime(1, false), "00:00");
	UASSERTEQ(std::string, formatDayCycleTime(0.9999999, false), "23:59");
	UASSERTEQ(std::string, formatDayCycleTime(0, true), "12:00 AM");
	UASSERTEQ(std::string, formatDayCycleTime(0.5, true), "12:00 PM");
	UASSERTEQ(std::string, formatDayCycleTime(13.0 / 24, true), "1:00 PM");
}

void TestDayCycle::testClockMinuteBoundaries(IGameDef *gamedef)
{
	for (unsigned minute = 0; minute < 1440; ++minute) {
		double time = minute / 1440.0;
		UASSERTEQ(unsigned, dayCycleMinute(time), minute);
		// Stay well outside the ULP-sized boundary tolerance.
		UASSERTEQ(unsigned, dayCycleMinute(time - 1e-8 / 1440), (minute + 1439) % 1440);
		UASSERTEQ(unsigned, dayCycleMinute(time + 1e-8 / 1440), minute);
		char expected24[6], expected12[12];
		std::snprintf(expected24, sizeof(expected24), "%02u:%02u", minute / 60, minute % 60);
		unsigned hour = minute / 60;
		std::snprintf(expected12, sizeof(expected12), "%u:%02u %s",
				hour % 12 ? hour % 12 : 12, minute % 60, hour < 12 ? "AM" : "PM");
		UASSERTEQ(std::string, formatDayCycleTime(time, false), expected24);
		UASSERTEQ(std::string, formatDayCycleTime(time, true), expected12);
	}
	UASSERTEQ(unsigned, dayCycleMinute(1), 0);
	UASSERTEQ(unsigned, dayCycleMinute(std::nextafter(1.0, 0.0)), 1439);
	EXCEPTION_CHECK(std::invalid_argument, dayCycleMinute(std::numeric_limits<double>::quiet_NaN()));
	EXCEPTION_CHECK(std::invalid_argument, dayCycleMinute(std::numeric_limits<double>::infinity()));

	ClockEnvironment env(gamedef);
	env.setDayCycle(exampleCycle());
	env.setWorldTime({7, 13.0 / 1440, true});
	env.stepTimeOfDay(60);
	UASSERTEQ(std::string, formatDayCycleTime(env.getTimeOfDayF(), false), "00:13");
	UASSERTEQ(unsigned, dayCycleMinute(env.getTimeOfDayF()), 13);
}
#endif
