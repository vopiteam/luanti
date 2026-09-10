// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

/*
	This class is the game's environment.
	It contains:
	- The map
	- Players
	- Other objects
	- The current time in the game
	- etc.
*/

#if IS_VOPI_ENGINE
#include "daycycle.h"
#endif
#include <atomic>
#include <mutex>
#include <optional>
#include "irr_v3d.h"
#include "util/basic_macros.h"
#include "line3d.h"

class IGameDef;
class Map;
struct PointedThing;
class RaycastState;
struct Pointabilities;

class Environment
{
public:
	// Environment will delete the map passed to the constructor
	Environment(IGameDef *gamedef);
	virtual ~Environment() = default;
	DISABLE_CLASS_COPY(Environment);

	/*
		Step everything in environment.
		- Move players
		- Step mobs
		- Run timers of map
	*/
	virtual void step(f32 dtime) = 0;

	virtual Map &getMap() = 0;

	u32 getDayNightRatio();
#if IS_VOPI_ENGINE
	void setDayCycleRenderTime(std::optional<double> time);
#endif

	// 0-23999
	virtual void setTimeOfDay(u32 time);
	u32 getTimeOfDay();
#if IS_VOPI_ENGINE
	double getTimeOfDayF();
	DayCycleSnapshot getDayCycleSnapshot();
	void setDayCycleSnapshot(const DayCycleSnapshot &snapshot);
	DayCycleTime getWorldTime();
	void setWorldTime(const DayCycleTime &time);
	void advanceTime(double game_seconds);
	void setDayCycle(const DayCycleDefinition &definition);
	DayCycleDefinition getDayCycle();
	DayCycleState getDayCycleState(std::optional<double> time = std::nullopt);
	void setDayCyclePaused(bool paused);
	double getTimeOfDaySpeed();
#else
	float getTimeOfDayF();
#endif

	void stepTimeOfDay(float dtime);

	void setTimeOfDaySpeed(float speed);

	void setDayNightRatioOverride(bool enable, u32 value);

	u32 getDayCount();

	/*!
	 * Returns false if the given line intersects with a
	 * non-air node, true otherwise.
	 * \param pos1 start of the line
	 * \param pos2 end of the line
	 * \param p output, position of the first non-air node
	 * the line intersects
	 */
	bool line_of_sight(v3f pos1, v3f pos2, v3s16 *p = nullptr);

	/*!
	 * Gets the objects pointed by the shootline as
	 * pointed things.
	 * If this is a client environment, the local player
	 * won't be returned.
	 * @param[in]  shootline_on_map the shootline for
	 * the test in world coordinates
	 *
	 * @param[out] objects          found objects
	 */
	virtual void getSelectedActiveObjects(const core::line3d<f32> &shootline_on_map,
			std::vector<PointedThing> &objects,
			const std::optional<Pointabilities> &pointabilities) = 0;

	/*!
	 * Returns the next node or object the shootline meets.
	 * @param state current state of the raycast
	 * @result output, will contain the next pointed thing
	 */
	void continueRaycast(RaycastState *state, PointedThing *result);

	// counter used internally when triggering ABMs
	u32 m_added_objects;

	IGameDef *getGameDef() { return m_gamedef; }

protected:
	std::atomic<float> m_time_of_day_speed;

	/*
	 * Below: values managed by m_time_lock
	 */
#if IS_VOPI_ENGINE
	DayCycleTime m_clock;
	DayCycleDefinition m_day_cycle;
	u32 m_day_cycle_revision = 1;
	u32 m_time_discontinuity = 0;
	std::optional<double> m_day_cycle_render_time;
	// Per-player rendering override; it does not change the clock or phase.
#else
	// Time of day in milli-hours (0-23999), determines day and night
	u32 m_time_of_day;
	// Time of day in 0...1
	float m_time_of_day_f;
	// Stores the skew created by the float -> u32 conversion
	// to be applied at next conversion, so that there is no real skew.
	float m_time_conversion_skew = 0.0f;
	// Overriding the day-night ratio is useful for custom sky visuals
#endif
	bool m_enable_day_night_ratio_override = false;
#if IS_VOPI_ENGINE
	u32 m_day_night_ratio_override = 0;
#else
	u32 m_day_night_ratio_override = 0.0f;
	// Days from the server start, accounts for time shift
	// in game (e.g. /time or bed usage)
	std::atomic<u32> m_day_count;
#endif
	/*
	 * Above: values managed by m_time_lock
	 */

	IGameDef *m_gamedef;

private:
	std::mutex m_time_lock;
};
