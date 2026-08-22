// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "lua_api/l_platform_state.h"
#include "lua_api/l_internal.h"
#include "common/c_content.h"
#include "platform_state.h"
#include "log.h"

#include <json/json.h>
#include <memory>

// get_platform_state(topic)
int ModApiPlatformState::l_get_platform_state(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::string topic = readParam<std::string>(L, 1);
	const std::string json = platform_state::get(topic);
	if (json.empty()) {
		lua_pushnil(L);
		return 1;
	}

	Json::Value root;
	{
		Json::CharReaderBuilder builder;
		// State documents are shallow by design; a runaway nesting is a
		// platform bug, not something to recurse into.
		builder.settings_["stackLimit"] = 64;
		builder.settings_["collectComments"] = false;
		std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		std::string errs;
		bool ok = false;
		try {
			ok = reader->parse(json.data(), json.data() + json.size(), &root, &errs);
		} catch (const Json::Exception &e) {
			// The bundled jsoncpp throws (not returns false) past stackLimit
			errs = e.what();
		}
		if (!ok) {
			errorstream << "get_platform_state(" << topic
				<< "): platform returned invalid JSON: " << errs << std::endl;
			lua_pushnil(L);
			return 1;
		}
	}

	// JSON null becomes nil, as core.parse_json does without a null value
	lua_pushnil(L);
	const int nullindex = lua_gettop(L);
	if (!push_json_value(L, root, nullindex)) {
		errorstream << "get_platform_state(" << topic
			<< "): document could not be converted" << std::endl;
		lua_pushnil(L);
	}
	return 1;
}

// platform_action(topic, action[, arg])
int ModApiPlatformState::l_platform_action(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	const std::string topic = readParam<std::string>(L, 1);
	const std::string action = readParam<std::string>(L, 2);
	std::string arg;
	const bool has_arg = !lua_isnoneornil(L, 3);
	if (has_arg)
		arg = readParam<std::string>(L, 3);
	lua_pushboolean(L, platform_state::action(topic, action,
			has_arg ? arg.c_str() : nullptr));
	return 1;
}

void ModApiPlatformState::Initialize(lua_State *L, int top)
{
	API_FCT(get_platform_state);
	API_FCT(platform_action);
}
