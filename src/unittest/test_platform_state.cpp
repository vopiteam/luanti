// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "test.h"

#include "platform_state.h"

#include <cstdlib>
#include <cstring>
#include <thread>

// The engine side of the platform state channel: behaviour without a
// provider, a fake provider round trip (document ownership, action
// arguments), and the change record the main-menu loop drains into menu
// events.
class TestPlatformState : public TestBase
{
public:
	TestPlatformState() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestPlatformState"; }

	void runTests(IGameDef *gamedef);

	void testNoPlatformDefaults();
	void testProviderRoundTrip();
	void testChangedTopicsCollapseAndDrain();
	void testIgnoresEmptyTopic();
	void testConcurrentMarks();
};

static TestPlatformState g_test_instance;

void TestPlatformState::runTests(IGameDef *gamedef)
{
	TEST(testNoPlatformDefaults);
	TEST(testProviderRoundTrip);
	TEST(testChangedTopicsCollapseAndDrain);
	TEST(testIgnoresEmptyTopic);
	TEST(testConcurrentMarks);
}

namespace {
	std::string g_last_action;

	// What a platform provider does: a malloc'd copy the engine frees
	char *malloc_copy(const char *text)
	{
		const size_t size = std::strlen(text) + 1;
		char *copy = static_cast<char *>(std::malloc(size));
		if (copy)
			std::memcpy(copy, text, size);
		return copy;
	}

	char *fake_state_json(const char *topic)
	{
		if (std::string(topic) != "demo")
			return nullptr;
		return malloc_copy("{\"generation\":3,\"packs\":[{\"id\":\"a\"}]}");
	}

	bool fake_action(const char *topic, const char *action, const char *arg)
	{
		g_last_action = std::string(topic) + "/" + action + "/" + (arg ? arg : "<null>");
		return std::string(topic) == "demo";
	}
}

void TestPlatformState::testNoPlatformDefaults()
{
	// No provider: no document, every action declined
	platform_state::setProvider(platform_state::Provider());
	UASSERT(platform_state::get("anything").empty());
	UASSERT(!platform_state::action("anything", "retry", nullptr));
	UASSERT(!platform_state::action("anything", "retry", "arg"));
}

void TestPlatformState::testProviderRoundTrip()
{
	platform_state::Provider provider;
	provider.state_json = fake_state_json;
	provider.action = fake_action;
	platform_state::setProvider(provider);

	// The document comes back verbatim (the malloc'd copy is freed by the
	// engine — checked by the sanitizer builds, not here)
	UASSERTEQ(std::string, platform_state::get("demo"),
			"{\"generation\":3,\"packs\":[{\"id\":\"a\"}]}");
	UASSERT(platform_state::get("other").empty());

	// Actions forward topic, action and the optional argument
	UASSERT(platform_state::action("demo", "retry", nullptr));
	UASSERTEQ(std::string, g_last_action, "demo/retry/<null>");
	UASSERT(platform_state::action("demo", "allow", "pack_x"));
	UASSERTEQ(std::string, g_last_action, "demo/allow/pack_x");
	UASSERT(!platform_state::action("other", "retry", nullptr));

	platform_state::setProvider(platform_state::Provider());
}

void TestPlatformState::testChangedTopicsCollapseAndDrain()
{
	platform_state::takeChanged(); // start clean

	porting_platform_state_changed("a");
	porting_platform_state_changed("b");
	porting_platform_state_changed("a"); // duplicate collapses, order kept

	std::vector<std::string> changed = platform_state::takeChanged();
	UASSERTEQ(size_t, changed.size(), 2);
	UASSERTEQ(std::string, changed[0], "a");
	UASSERTEQ(std::string, changed[1], "b");

	// Drained: nothing until the next change
	UASSERT(platform_state::takeChanged().empty());
	porting_platform_state_changed("b");
	changed = platform_state::takeChanged();
	UASSERTEQ(size_t, changed.size(), 1);
	UASSERTEQ(std::string, changed[0], "b");
}

void TestPlatformState::testIgnoresEmptyTopic()
{
	platform_state::takeChanged();
	porting_platform_state_changed(nullptr);
	porting_platform_state_changed("");
	UASSERT(platform_state::takeChanged().empty());
}

void TestPlatformState::testConcurrentMarks()
{
	platform_state::takeChanged();
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++) {
		threads.emplace_back([t]() {
			const std::string topic = "topic" + std::to_string(t % 2);
			for (int i = 0; i < 1000; i++)
				platform_state::markChanged(topic);
		});
	}
	for (std::thread &th : threads)
		th.join();
	UASSERTEQ(size_t, platform_state::takeChanged().size(), 2);
}
