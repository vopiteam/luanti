// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "platform_state.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace {

// porting_platform_state_changed() is called from platform threads at any
// time — including while static initialization is still running, and
// after exit() has begun tearing static objects down (a download that is
// still publishing when the app terminates). A function-local, never
// destroyed instance is valid at both ends; the leak is one object.
struct Registry {
	std::mutex mutex;
	std::vector<std::string> changed;
	platform_state::Provider provider;
};

Registry &registry()
{
	static Registry *instance = new Registry();
	return *instance;
}

}

extern "C" void porting_platform_state_changed(const char *topic)
{
	if (topic && *topic)
		platform_state::markChanged(topic);
}

namespace platform_state
{

void setProvider(const Provider &provider)
{
	Registry &r = registry();
	std::lock_guard<std::mutex> lock(r.mutex);
	r.provider = provider;
}

std::string get(const std::string &topic)
{
	Provider provider;
	{
		Registry &r = registry();
		std::lock_guard<std::mutex> lock(r.mutex);
		provider = r.provider;
	}
	if (!provider.state_json)
		return "";
	char *json = provider.state_json(topic.c_str());
	if (!json)
		return "";
	std::string result(json);
	std::free(json);
	return result;
}

bool action(const std::string &topic, const std::string &action, const char *arg)
{
	Provider provider;
	{
		Registry &r = registry();
		std::lock_guard<std::mutex> lock(r.mutex);
		provider = r.provider;
	}
	if (!provider.action)
		return false;
	return provider.action(topic.c_str(), action.c_str(), arg);
}

void markChanged(const std::string &topic)
{
	Registry &r = registry();
	std::lock_guard<std::mutex> lock(r.mutex);
	if (std::find(r.changed.begin(), r.changed.end(), topic) == r.changed.end())
		r.changed.push_back(topic);
}

std::vector<std::string> takeChanged()
{
	Registry &r = registry();
	std::lock_guard<std::mutex> lock(r.mutex);
	std::vector<std::string> out;
	out.swap(r.changed);
	return out;
}

}
