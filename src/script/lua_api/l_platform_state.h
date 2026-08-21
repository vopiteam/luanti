// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include "lua_api/l_base.h"

/*
	Main menu view of the platform state channel (VOPI Engine), see
	src/platform_state.h. Registered in the main menu environment only: the
	hooks call into the platform layer, which is meant to be asked from the
	menu loop, not from server or async threads.

	    local state = core.get_platform_state("some_topic")
	    if state then ... end       -- nil: no such topic, or no platform layer
	    core.platform_action("some_topic", "retry")

	Menu event "PlatformStateChange:<topic>" announces a changed topic.
*/
class ModApiPlatformState : public ModApiBase
{
private:
	// get_platform_state(topic) -> table or nil
	static int l_get_platform_state(lua_State *L);

	// platform_action(topic, action[, arg]) -> bool
	static int l_platform_action(lua_State *L);

public:
	static void Initialize(lua_State *L, int top);
};
