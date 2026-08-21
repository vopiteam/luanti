// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "test.h"

#include "platform_state.h"

#include <thread>

// The engine side of the platform state channel: the default hooks of a
// build without a platform layer, and the change record the main-menu loop
// drains into menu events.
class TestPlatformState : public TestBase
{
public:
	TestPlatformState() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestPlatformState"; }

	void runTests(IGameDef *gamedef);

	void testNoPlatformDefaults();
	void testChangedTopicsCollapseAndDrain();
	void testIgnoresEmptyTopic();
	void testConcurrentMarks();
};

static TestPlatformState g_test_instance;

void TestPlatformState::runTests(IGameDef *gamedef)
{
	TEST(testNoPlatformDefaults);
	TEST(testChangedTopicsCollapseAndDrain);
	TEST(testIgnoresEmptyTopic);
	TEST(testConcurrentMarks);
}

void TestPlatformState::testNoPlatformDefaults()
{
	// Weak defaults: no document, every action declined
	UASSERT(platform_state::get("anything").empty());
	UASSERT(!platform_state::action("anything", "retry", nullptr));
	UASSERT(!platform_state::action("anything", "retry", "arg"));
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
