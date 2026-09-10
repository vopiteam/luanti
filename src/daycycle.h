// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#if IS_VOPI_ENGINE
#include <cstdint>
#include <string>

// All positions are fractions of a civil day; durations are simulation seconds.
struct DayCycleDefinition {
	bool enabled = false;
	double day_start = 0.25;
	double night_start = 0.75;
	double day_duration = 600;
	double night_duration = 600;
	double dawn_start = 5.0 / 24;
	double sunrise = 0.25;
	double dawn_end = 7.0 / 24;
	double dusk_start = 17.0 / 24;
	double sunset = 0.75;
	double dusk_end = 19.0 / 24;
	double night_light = 0.175;
	double day_light = 1.0;

	void validate() const;
	bool isDay(double time) const;
	double speed(double time) const;
	std::string serialize() const;
	static DayCycleDefinition deserialize(const std::string &value);
};

struct DayCycleState {
	double timeofday = 0;
	bool is_day = false;
	bool is_dawn = false;
	bool is_dusk = false;
	double daylight = 0;
	double day_night_ratio = 0;
	double day_weight = 0;
	double dawn_weight = 0;
	double night_weight = 1;
	double orbit_time = 0;
	double shadow_factor = 0;
};

double dayCycleWrap(double time);
double legacyDayCycleOrbit(double time);
double legacyDayCycleShadow(double time);
// Shared by clock text and its minute-change cache.
unsigned dayCycleMinute(double time);
std::string formatDayCycleTime(double time, bool twelve_hour);
DayCycleState evaluateDayCycle(const DayCycleDefinition &definition, double time);

// The fractional clock is authoritative. Integer time is a derived view.
struct DayCycleTime {
	uint32_t day = 0;
	double timeofday = 0;
	bool paused = false;

	void validate() const;
	void advanceDays(double days);
	void advance(double seconds, const DayCycleDefinition &definition,
			double legacy_speed);
};

// Versioned, bounded representation shared by persistence and the wire extension.
constexpr uint32_t DAY_CYCLE_CAPABILITY = 0x44435943; // DCYC
constexpr uint16_t DAY_CYCLE_PROTOCOL = 1;
struct DayCycleSnapshot {
	DayCycleDefinition definition;
	DayCycleTime clock;
	uint32_t revision = 1;
	uint32_t discontinuity = 0;

	std::string serialize() const;
	static DayCycleSnapshot deserialize(const std::string &value);
};

#endif
