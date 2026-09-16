// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <SColor.h>
#include "skyparams.h"
#include <algorithm>
#include <cmath>

// Client appearance helpers deliberately do not smooth the day-cycle clock.
namespace SkyAppearance {

inline float retention(float dtime, float duration)
{
	return std::exp(-std::max(dtime, 0.0f) / duration);
}

// The palette a controlled regular sky shows follows the palette the server
// sent last with a 0.5-second exponential time constant: a game that gives
// every biome its own sky sends one target per change and the client eases
// into it, fog opacity included. The clock weights blend the followed
// colors, so time stays current. The first sample after a reset shows at
// once.
class Palette {
public:
	enum Slot : u8 {
		DAY_SKY, DAY_HORIZON, DAWN_SKY, DAWN_HORIZON, NIGHT_SKY, NIGHT_HORIZON,
		INDOORS, FOG, SUN_TINT, MOON_TINT, SLOT_COUNT
	};
	static constexpr float TIME_CONSTANT = 0.5f;

	void reset() { m_initialized = false; }
	bool initialized() const { return m_initialized; }

	void update(const SkyColor &sky, video::SColor fog, video::SColor sun_tint,
			video::SColor moon_tint, float dtime)
	{
		const video::SColorf targets[SLOT_COUNT] = {
			video::SColorf(sky.day_sky), video::SColorf(sky.day_horizon),
			video::SColorf(sky.dawn_sky), video::SColorf(sky.dawn_horizon),
			video::SColorf(sky.night_sky), video::SColorf(sky.night_horizon),
			video::SColorf(sky.indoors), video::SColorf(fog),
			video::SColorf(sun_tint), video::SColorf(moon_tint),
		};
		if (!m_initialized) {
			std::copy(targets, targets + SLOT_COUNT, m_current);
			m_initialized = true;
			return;
		}
		if (dtime <= 0)
			return;
		const float keep = retention(dtime, TIME_CONSTANT);
		for (int i = 0; i < SLOT_COUNT; ++i)
			m_current[i] = follow(m_current[i], targets[i], keep);
	}

	const video::SColorf &color(Slot slot) const { return m_current[slot]; }
	video::SColor fog() const { return m_current[FOG].toSColor(); }

private:
	static float channel(float current, float target, float keep)
	{
		const float next = target + (current - target) * keep;
		// Below a quarter of an 8-bit step the tail is invisible: land exactly.
		return std::abs(next - target) < 1.0f / 1024 ? target : next;
	}
	static video::SColorf follow(const video::SColorf &current,
			const video::SColorf &target, float keep)
	{
		return video::SColorf(channel(current.r, target.r, keep),
				channel(current.g, target.g, keep),
				channel(current.b, target.b, keep),
				channel(current.a, target.a, keep));
	}

	bool m_initialized = false;
	video::SColorf m_current[SLOT_COUNT];
};

class Exposure {
public:
	void reset() { m_initialized = false; }

	void update(bool sunlight, float direct_brightness, float dtime)
	{
		if (!m_initialized) {
			m_indoors = sunlight ? 0.0f : 1.0f;
			m_cave_brightness = direct_brightness;
			m_initialized = true;
			return;
		}
		if (dtime <= 0 || (sunlight && m_indoors == 0))
			return;
		const float keep = retention(dtime, 0.4f);
		if (!sunlight) {
			// At zero indoor contribution this cannot change the displayed value.
			// During a reversal retain the old sample, avoiding an exposure jump.
			if (m_indoors == 0)
				m_cave_brightness = direct_brightness;
			else
				m_cave_brightness = direct_brightness +
						(m_cave_brightness - direct_brightness) * keep;
		}
		const float target = sunlight ? 0.0f : 1.0f;
		m_indoors = target + (m_indoors - target) * keep;
		if (std::abs(m_indoors - target) < 0.000001f)
			m_indoors = target;
	}

	float indoors() const { return m_indoors; }
	float brightness(float outdoor_brightness) const
	{
		return outdoor_brightness * (1 - m_indoors) + m_cave_brightness * m_indoors;
	}
	video::SColorf color(video::SColorf outdoors, video::SColorf indoors) const
	{
		return indoors.getInterpolated(outdoors, m_indoors);
	}

private:
	bool m_initialized = false;
	float m_indoors = 0;
	float m_cave_brightness = 0;
};

inline video::SColor fogColor(video::SColor fog, video::SColor background,
		float brightness, bool controlled)
{
	if (fog.getAlpha() == 0)
		return background;
	if (!controlled)
		return fog;
	video::SColorf custom(fog);
	custom.r *= brightness;
	custom.g *= brightness;
	custom.b *= brightness;
	const float strength = custom.a;
	custom.a = 1;
	return custom.getInterpolated(video::SColorf(background), strength).toSColor();
}

inline float fogDistance(float end)
{
	return end > 0 ? end : 1.0f;
}

class Fog {
public:
	void update(float range, float start, float limit, float dtime, bool enabled)
	{
		if (!enabled || !m_active) {
			m_range = range;
			m_start = start;
		} else if (dtime > 0 && (m_range != range || m_start != start)) {
			const double keep = std::exp(-static_cast<double>(dtime) / 0.2);
			m_range = range + (m_range - range) * keep;
			m_start = start + (m_start - start) * keep;
			// Double state avoids a float-resolution plateau before convergence.
			if (std::abs(m_range - range) < std::max(1.0f, range) * 0.000001f)
				m_range = range;
			if (std::abs(m_start - start) < 0.000001f)
				m_start = start;
		}
		m_range = std::min(m_range, static_cast<double>(limit));
		m_active = enabled;
	}
	bool active() const { return m_active; }
	float range() const { return m_range; }
	float start() const { return m_start; }

private:
	bool m_active = false;
	double m_range = 0;
	double m_start = 0;
};

} // namespace SkyAppearance
