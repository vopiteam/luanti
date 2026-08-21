// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include <string>
#include <vector>

/*
	Platform state channel (VOPI Engine).

	The platform layer around the engine (the mobile app) runs services whose
	state the main menu wants to show — the progress of a background content
	download, for instance. Instead of one C hook per value, the platform
	publishes an opaque JSON document per *topic*, which menu Lua reads with
	core.get_platform_state(topic); simple commands travel the other way
	through core.platform_action(topic, action, arg). Topics, fields and
	actions are a contract between the platform code and the menu scripts —
	the engine carries them without interpreting anything.

	Change notification: the platform calls porting_platform_state_changed()
	from any thread; the main-menu loop drains the changed set once per frame
	and raises one "PlatformStateChange:<topic>" menu event per topic (the
	same path as "WindowInfoChange"), so Lua re-reads the topic instead of
	polling. Changes made while a world is running are delivered on the first
	menu frame after it.

	Intended for state snapshots and occasional commands: documents of a few
	kilobytes, an event only when something actually changed. Not a
	streaming channel.

	The two platform hooks are weak symbols with no-op defaults, so a build
	without a platform layer (desktop, tests) reports no state and declines
	every action; a platform provides strong definitions.
*/

extern "C" {
	// JSON document describing the current state of `topic`, allocated with
	// malloc (strdup) — the engine frees it. nullptr: unknown topic or no
	// platform layer.
	char *vopi_platform_state_json(const char *topic);

	// Forward a command to the platform. `arg` may be nullptr. Returns
	// whether the platform accepted it.
	bool vopi_platform_action(const char *topic, const char *action,
			const char *arg);

	// The platform changed the state of `topic`. Any thread.
	void porting_platform_state_changed(const char *topic);
}

namespace platform_state
{
	// Current JSON document of a topic, empty when there is none.
	std::string get(const std::string &topic);

	// Send a command; `arg` may be nullptr.
	bool action(const std::string &topic, const std::string &action,
			const char *arg);

	// Record a change of `topic` (any thread; duplicates collapse).
	void markChanged(const std::string &topic);

	// Take the topics changed since the previous call, oldest first, and
	// clear the record. Main thread.
	std::vector<std::string> takeChanged();
}
