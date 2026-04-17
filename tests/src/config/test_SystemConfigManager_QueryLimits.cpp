/**
 * @file test_SystemConfigManager_QueryLimits.cpp
 * @brief Tests for query safety limit configuration getters.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "graph/config/SystemConfigManager.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

using namespace graph;
using namespace graph::config;
using namespace graph::storage::state;

class SystemConfigManagerQueryLimitsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_queryLimits_" + to_string(uuid) + ".dat");

		database = std::make_unique<Database>(testFilePath.string());
		database->open();

		stateManager = std::make_shared<SystemStateManager>(database->getStorage()->getDataManager());
		configManager = std::make_shared<SystemConfigManager>(stateManager, database->getStorage());
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::error_code ec;
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath, ec);
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<Database> database;
	std::shared_ptr<SystemStateManager> stateManager;
	std::shared_ptr<SystemConfigManager> configManager;
};

TEST_F(SystemConfigManagerQueryLimitsTest, DefaultQueryTimeoutMs) {
	EXPECT_EQ(configManager->getQueryTimeoutMs(), 30000);
}

TEST_F(SystemConfigManagerQueryLimitsTest, DefaultQueryMaxMemoryMb) {
	EXPECT_EQ(configManager->getQueryMaxMemoryMb(), 0);
}

TEST_F(SystemConfigManagerQueryLimitsTest, DefaultQueryMaxVarLengthDepth) {
	EXPECT_EQ(configManager->getQueryMaxVarLengthDepth(), 50);
}

TEST_F(SystemConfigManagerQueryLimitsTest, SetAndGetQueryTimeoutMs) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::QUERY_TIMEOUT_MS, 5000);
	EXPECT_EQ(configManager->getQueryTimeoutMs(), 5000);
}

TEST_F(SystemConfigManagerQueryLimitsTest, SetAndGetQueryMaxMemoryMb) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::QUERY_MAX_MEMORY_MB, 512);
	EXPECT_EQ(configManager->getQueryMaxMemoryMb(), 512);
}

TEST_F(SystemConfigManagerQueryLimitsTest, SetAndGetQueryMaxVarLengthDepth) {
	stateManager->set<int64_t>(keys::SYS_CONFIG, keys::Config::QUERY_MAX_VAR_LENGTH_DEPTH, 10);
	EXPECT_EQ(configManager->getQueryMaxVarLengthDepth(), 10);
}
