// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#if IS_VOPI_ENGINE
#include "daycycle.h"
#include "irrlichttypes.h"
#include "daynightratio.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {
double smooth(double a, double b, double value)
{
	double t = std::clamp((value - a) / (b - a), 0.0, 1.0);
	return t * t * (3 - 2 * t);
}

void require(bool valid, const char *message)
{
	if (!valid)
		throw std::invalid_argument(message);
}
}

double dayCycleWrap(double time)
{
	return time - std::floor(time);
}

void DayCycleDefinition::validate() const
{
	for (double t : {day_start, night_start, dawn_start, sunrise, dawn_end,
			dusk_start, sunset, dusk_end})
		require(std::isfinite(t) && t >= 0 && t < 1,
				"day cycle positions must be finite and in [0, 1)");
	double day_length = dayCycleWrap(night_start - day_start);
	require(day_length >= 1.0 / 86400 && day_length <= 1 - 1.0 / 86400,
			"day cycle intervals must span at least one game second");
	for (double duration : {day_duration, night_duration})
		require(std::isfinite(duration) && duration >= 1 && duration <= 31536000,
				"day cycle durations must be between 1 and 31536000 seconds");
	double previous = 0;
	for (double t : {sunrise, dawn_end, dusk_start, sunset, dusk_end}) {
		double offset = dayCycleWrap(t - dawn_start);
		require(offset - previous >= 1.0 / 86400,
				"day cycle visual positions must be in cyclic order");
		previous = offset;
	}
	require(1 - previous >= 1.0 / 86400, "day cycle requires a night interval");
	require(std::isfinite(night_light) && std::isfinite(day_light) &&
			night_light >= 0 && day_light <= 1 && night_light < day_light,
			"day cycle light must satisfy 0 <= night < day <= 1");
}

bool DayCycleDefinition::isDay(double time) const
{
	return dayCycleWrap(time - day_start) < dayCycleWrap(night_start - day_start);
}

double DayCycleDefinition::speed(double time) const
{
	double length = dayCycleWrap(night_start - day_start);
	return 86400 * (isDay(time) ? length / day_duration :
			(1 - length) / night_duration);
}

std::string DayCycleDefinition::serialize() const
{
	std::ostringstream os;
	os.imbue(std::locale::classic());
	os << std::setprecision(17) << 1 << ' ' << enabled;
	for (double value : {day_start, night_start, day_duration, night_duration,
			dawn_start, sunrise, dawn_end, dusk_start, sunset, dusk_end,
			night_light, day_light})
		os << ' ' << value;
	return os.str();
}

DayCycleDefinition DayCycleDefinition::deserialize(const std::string &value)
{
	require(value.size() <= 1024, "day cycle definition is too large");
	std::istringstream is(value);
	is.imbue(std::locale::classic());
	DayCycleDefinition d;
	unsigned version, enabled;
	require(bool(is >> version >> enabled) && version == 1 && enabled <= 1,
			"unsupported day cycle definition");
	d.enabled = enabled;
	for (double *field : {&d.day_start, &d.night_start, &d.day_duration,
			&d.night_duration, &d.dawn_start, &d.sunrise, &d.dawn_end,
			&d.dusk_start, &d.sunset, &d.dusk_end, &d.night_light, &d.day_light})
		require(bool(is >> *field), "invalid day cycle definition");
	is >> std::ws;
	require(is.eof(), "trailing data in day cycle definition");
	d.validate();
	return d;
}

unsigned dayCycleMinute(double time)
{
	require(std::isfinite(time), "clock display time must be finite");
	double minutes = dayCycleWrap(time) * 1440;
	double boundary = std::round(minutes);
	// A normalized exact minute can multiply back to just below its integer.
	// Correct only floating-point noise, never round ordinary seconds forward.
	double tolerance = 4 * std::numeric_limits<double>::epsilon() *
			std::max(1.0, minutes);
	if (std::abs(minutes - boundary) <= tolerance)
		minutes = boundary;
	// Values just below midnight still belong to 23:59; time == 1 wraps to 0.
	return std::min(1439u, static_cast<unsigned>(minutes));
}

std::string formatDayCycleTime(double time, bool twelve_hour)
{
	unsigned minute = dayCycleMinute(time);
	unsigned hour = minute / 60;
	std::ostringstream os;
	os.imbue(std::locale::classic());
	if (twelve_hour)
		os << (hour % 12 == 0 ? 12 : hour % 12);
	else
		os << std::setfill('0') << std::setw(2) << hour;
	os << ':' << std::setfill('0') << std::setw(2) << minute % 60;
	if (twelve_hour)
		os << (hour < 12 ? " AM" : " PM");
	return os.str();
}

double legacyDayCycleOrbit(double time)
{
	// Preserve the original float arithmetic of the standard renderer.
	float t = time;
	float wn = 0.415f / 2;
	if (t > wn && t < 1.0f - wn)
		return (t - wn) / (1.0f - wn * 2) * 0.5f + 0.25f;
	if (t < 0.5f)
		return t / wn * 0.25f;
	return 1.0f - ((1.0f - t) / wn * 0.25f);
}

double legacyDayCycleShadow(double time)
{
	if (time < 0.2)
		return 0.5 * (1 - smooth(0.18, 0.2, time));
	if (time >= 0.8)
		return 0.5 * smooth(0.8, 0.83, time);
	return smooth(0.20, 0.25, time) * (1 - smooth(0.7, 0.8, time));
}

DayCycleState evaluateDayCycle(const DayCycleDefinition &d, double time)
{
	require(std::isfinite(time), "day cycle time must be finite");
	DayCycleState s;
	s.timeofday = dayCycleWrap(time);
	s.is_day = d.isDay(s.timeofday);
	if (!d.enabled) {
		s.day_night_ratio = time_to_daynight_ratio(s.timeofday * 24000, true) / 1000.0;
		s.daylight = std::clamp((s.day_night_ratio - 0.175) / 0.825, 0.0, 1.0);
		s.day_weight = s.daylight;
		s.night_weight = 1 - s.daylight;
		s.orbit_time = legacyDayCycleOrbit(s.timeofday);
		s.shadow_factor = legacyDayCycleShadow(s.timeofday);
		return s;
	}
	double t = dayCycleWrap(s.timeofday - d.dawn_start);
	double rise = dayCycleWrap(d.sunrise - d.dawn_start);
	double dawn_end = dayCycleWrap(d.dawn_end - d.dawn_start);
	double dusk_start = dayCycleWrap(d.dusk_start - d.dawn_start);
	double set = dayCycleWrap(d.sunset - d.dawn_start);
	double dusk_end = dayCycleWrap(d.dusk_end - d.dawn_start);
	s.is_dawn = t < dawn_end;
	s.is_dusk = t >= dusk_start && t < dusk_end;
	if (s.is_dawn) {
		s.daylight = smooth(0, dawn_end, t);
		if (t < rise) {
			s.dawn_weight = smooth(0, rise, t);
			s.night_weight = 1 - s.dawn_weight;
		} else {
			s.day_weight = smooth(rise, dawn_end, t);
			s.dawn_weight = 1 - s.day_weight;
			s.night_weight = 0;
		}
	} else if (t < dusk_start) {
		s.daylight = s.day_weight = 1;
		s.night_weight = 0;
	} else if (s.is_dusk) {
		s.daylight = 1 - smooth(dusk_start, dusk_end, t);
		if (t < set) {
			s.dawn_weight = smooth(dusk_start, set, t);
			s.day_weight = 1 - s.dawn_weight;
			s.night_weight = 0;
		} else {
			s.night_weight = smooth(set, dusk_end, t);
			s.dawn_weight = 1 - s.night_weight;
		}
	}
	s.day_night_ratio = d.night_light + s.daylight * (d.day_light - d.night_light);
	double solar_day = dayCycleWrap(d.sunset - d.sunrise);
	double solar_offset = dayCycleWrap(s.timeofday - d.sunrise);
	s.orbit_time = dayCycleWrap(solar_offset < solar_day ?
			0.25 + 0.5 * solar_offset / solar_day :
			0.75 + 0.5 * (solar_offset - solar_day) / (1 - solar_day));
	// Fade each light to zero at the horizon before changing the shadow caster.
	double altitude = std::abs(std::sin((s.orbit_time - 0.25) * 2 * 3.141592653589793));
	s.shadow_factor = smooth(0, 0.25, altitude) *
			(solar_offset < solar_day ? 1.0 : 0.5);
	return s;
}

void DayCycleTime::validate() const
{
	require(std::isfinite(timeofday) && timeofday >= 0 && timeofday < 1,
			"world time must be finite and in [0, 1)");
}

void DayCycleTime::advanceDays(double days)
{
	require(std::isfinite(days) && days >= 0, "time advance must be finite and nonnegative");
	double total = timeofday + days;
	double crossed = std::floor(total);
	require(crossed <= std::numeric_limits<uint32_t>::max() - day,
			"world day counter overflow");
	day += static_cast<uint32_t>(crossed);
	timeofday = total - crossed;
}

void DayCycleTime::advance(double seconds, const DayCycleDefinition &d,
		double legacy_speed)
{
	require(std::isfinite(seconds) && seconds >= 0, "elapsed time must be finite and nonnegative");
	if (paused || seconds == 0)
		return;
	if (!d.enabled) {
		require(std::isfinite(legacy_speed) && legacy_speed >= 0,
				"time speed must be finite and nonnegative");
		advanceDays(seconds * legacy_speed / 86400);
		return;
	}
	double length = dayCycleWrap(d.night_start - d.day_start);
	double offset = dayCycleWrap(timeofday - d.day_start);
	double elapsed = offset < length ? offset / length * d.day_duration :
			d.day_duration + (offset - length) / (1 - length) * d.night_duration;
	double total = elapsed + seconds;
	double period = d.day_duration + d.night_duration;
	double cycles = std::floor(total / period);
	double remainder = total - cycles * period;
	double next_offset = remainder < d.day_duration ?
			remainder / d.day_duration * length :
			length + (remainder - d.day_duration) / d.night_duration * (1 - length);
	advanceDays(std::max(0.0, cycles + next_offset - offset));
}

std::string DayCycleSnapshot::serialize() const
{
	std::ostringstream os;
	os.imbue(std::locale::classic());
	os << std::setprecision(17) << 1 << ' ' << revision << ' ' << discontinuity
			<< ' ' << clock.day << ' ' << clock.timeofday << ' ' << clock.paused
			<< ' ' << std::quoted(definition.serialize());
	return os.str();
}

DayCycleSnapshot DayCycleSnapshot::deserialize(const std::string &value)
{
	require(value.size() <= 2048, "day cycle snapshot is too large");
	std::istringstream is(value);
	is.imbue(std::locale::classic());
	uint64_t version, revision, discontinuity, day, paused;
	DayCycleSnapshot s;
	std::string definition;
	require(bool(is >> version >> revision >> discontinuity >> day >> s.clock.timeofday
			>> paused >> std::quoted(definition)), "invalid day cycle snapshot");
	require(version == 1 && revision > 0 && revision <= UINT32_MAX &&
			discontinuity <= UINT32_MAX && day <= UINT32_MAX && paused <= 1,
			"unsupported or invalid day cycle snapshot");
	is >> std::ws;
	require(is.eof(), "trailing data in day cycle snapshot");
	s.revision = revision;
	s.discontinuity = discontinuity;
	s.clock.day = day;
	s.clock.paused = paused;
	s.clock.validate();
	s.definition = DayCycleDefinition::deserialize(definition);
	return s;
}

#endif
