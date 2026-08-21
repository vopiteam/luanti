// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "platform_state.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

// Defaults for builds without a platform layer; a platform overrides both
// with strong definitions (see the header).
extern "C" __attribute__((weak)) char *vopi_platform_state_json(const char *)
{
	return nullptr;
}

extern "C" __attribute__((weak)) bool vopi_platform_action(const char *,
		const char *, const char *)
{
	return false;
}

extern "C" void porting_platform_state_changed(const char *topic)
{
	if (topic && *topic)
		platform_state::markChanged(topic);
}

namespace {
	std::mutex g_changed_mutex;
	std::vector<std::string> g_changed;
}

namespace platform_state
{

std::string get(const std::string &topic)
{
	char *json = vopi_platform_state_json(topic.c_str());
	if (!json)
		return "";
	std::string result(json);
	std::free(json);
	return result;
}

bool action(const std::string &topic, const std::string &action, const char *arg)
{
	return vopi_platform_action(topic.c_str(), action.c_str(), arg);
}

void markChanged(const std::string &topic)
{
	std::lock_guard<std::mutex> lock(g_changed_mutex);
	if (std::find(g_changed.begin(), g_changed.end(), topic) == g_changed.end())
		g_changed.push_back(topic);
}

std::vector<std::string> takeChanged()
{
	std::lock_guard<std::mutex> lock(g_changed_mutex);
	std::vector<std::string> out;
	out.swap(g_changed);
	return out;
}

}
