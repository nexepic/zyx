/**
 * @file test_SlowQueryLog.cpp
 * @date 2026/04/02
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/config/SystemConfigManager.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

using namespace graph;
using namespace graph::config;
using namespace graph::storage::state;
namespace fs = std::filesystem;

class SlowQueryLogTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_slowlog_" + to_string(uuid) + ".dat");

		database = std::make_unique<Database>(testFilePath.string());
		database->open();

		stateManager = std::make_shared<SystemStateManager>(database->getStorage()->getDataManager());
		configManager = std::make_shared<SystemConfigManager>(stateManager, database->getStorage());
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath, ec);
		}
	}

	fs::path testFilePath;
	std::unique_ptr<Database> database;
	std::shared_ptr<SystemStateManager> stateManager;
	std::shared_ptr<SystemConfigManager> configManager;
};

// Test 1: Slow log is disabled by default
TEST_F(SlowQueryLogTest, DefaultDisabled) {
	EXPECT_FALSE(configManager->isSlowLogEnabled());
}

// Test 2: Default threshold is 1000ms
TEST_F(SlowQueryLogTest, DefaultThreshold) {
	EXPECT_EQ(configManager->getSlowLogThresholdMs(), 1000);
}

// Test 3: Enable slow log via config
TEST_F(SlowQueryLogTest, EnableSlowLog) {
	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, true);
	EXPECT_TRUE(configManager->isSlowLogEnabled());
}

// Test 4: Disable slow log via config
TEST_F(SlowQueryLogTest, DisableSlowLog) {
	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, true);
	EXPECT_TRUE(configManager->isSlowLogEnabled());

	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, false);
	EXPECT_FALSE(configManager->isSlowLogEnabled());
}

// Test 5: Set custom threshold
TEST_F(SlowQueryLogTest, CustomThreshold) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 500);
	EXPECT_EQ(configManager->getSlowLogThresholdMs(), 500);
}

// Test 6: Set threshold to zero (log every query)
TEST_F(SlowQueryLogTest, ZeroThreshold) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 0);
	EXPECT_EQ(configManager->getSlowLogThresholdMs(), 0);
}

// Test 7: Set threshold to large value
TEST_F(SlowQueryLogTest, LargeThreshold) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 60000);
	EXPECT_EQ(configManager->getSlowLogThresholdMs(), 60000);
}

// Test 8: Slow log config accessible via Database::getConfigManager
TEST_F(SlowQueryLogTest, AccessibleViaDatabase) {
	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, true);
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 200);

	auto cm = database->getConfigManager();
	ASSERT_NE(cm, nullptr);
	EXPECT_TRUE(cm->isSlowLogEnabled());
	EXPECT_EQ(cm->getSlowLogThresholdMs(), 200);
}

// Test 9: Stats enabled by default
TEST_F(SlowQueryLogTest, StatsEnabledByDefault) {
	EXPECT_TRUE(configManager->isStatsEnabled());
}

// Test 10: Disable stats collection
TEST_F(SlowQueryLogTest, DisableStats) {
	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::STATS_ENABLED, false);
	EXPECT_FALSE(configManager->isStatsEnabled());
}

// Test 11: Persistence through restart
TEST_F(SlowQueryLogTest, PersistenceThroughRestart) {
	stateManager->set<bool>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_ENABLED, true);
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::SLOW_LOG_THRESHOLD_MS, 250);

	database->getStorage()->flush();
	database->close();

	auto newDatabase = std::make_unique<Database>(testFilePath.string());
	newDatabase->open();

	auto newStateManager = std::make_shared<SystemStateManager>(newDatabase->getStorage()->getDataManager());
	auto newConfigManager = std::make_shared<SystemConfigManager>(newStateManager, newDatabase->getStorage());

	EXPECT_TRUE(newConfigManager->isSlowLogEnabled());
	EXPECT_EQ(newConfigManager->getSlowLogThresholdMs(), 250);

	newDatabase->close();
}

// Test 12: Slow log config via CALL dbms.setConfig Cypher
TEST_F(SlowQueryLogTest, SetConfigViaCypher) {
	auto qe = database->getQueryEngine();

	auto result = qe->execute("CALL dbms.setConfig('query.slow_log.enabled', true)");
	EXPECT_FALSE(result.isEmpty());

	EXPECT_TRUE(configManager->isSlowLogEnabled());
}
