/**
 * @file test_StateRegistry.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include "graph/core/Database.hpp"
#include "graph/core/State.hpp"
#include "graph/core/StateRegistry.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class StateRegistryTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a unique temp file for the database
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_stateRegistry_" + to_string(uuid) + ".dat");

		// Initialize Database and DataManager
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Add active states
		graph::State s1(1, "int_key", "42");
		graph::State s2(2, "double_key", "3.14");
		graph::State s3(3, "bool_key_true", "true");
		graph::State s4(4, "bool_key_false", "false");
		graph::State s5(5, "string_key", "hello");
		dataManager->addStateEntity(s1);
		dataManager->addStateEntity(s2);
		dataManager->addStateEntity(s3);
		dataManager->addStateEntity(s4);
		dataManager->addStateEntity(s5);

		// Add inactive state
		graph::State inactiveState(6, "inactive_key", "should_not_load");
		dataManager->addStateEntity(inactiveState);
		dataManager->deleteState(inactiveState);

		graph::StateRegistry::initialize(dataManager);
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
};

TEST_F(StateRegistryTest, GetStringReturnsCorrectValue) {
	EXPECT_EQ(graph::StateRegistry::getString("string_key", "default"), "hello");
	EXPECT_EQ(graph::StateRegistry::getString("not_exist", "default"), "default");
}

TEST_F(StateRegistryTest, GetIntReturnsCorrectValue) {
	EXPECT_EQ(graph::StateRegistry::getInt("int_key", 0), 42);
	EXPECT_EQ(graph::StateRegistry::getInt("not_exist", 123), 123);
	// Invalid string
	graph::StateRegistry::setString("bad_int", "abc");
	EXPECT_EQ(graph::StateRegistry::getInt("bad_int", 99), 99);
}

TEST_F(StateRegistryTest, GetDoubleReturnsCorrectValue) {
	EXPECT_DOUBLE_EQ(graph::StateRegistry::getDouble("double_key", 0.0), 3.14);
	EXPECT_DOUBLE_EQ(graph::StateRegistry::getDouble("not_exist", 2.71), 2.71);
	graph::StateRegistry::setString("bad_double", "abc");
	EXPECT_DOUBLE_EQ(graph::StateRegistry::getDouble("bad_double", 1.23), 1.23);
}

TEST_F(StateRegistryTest, GetBoolReturnsCorrectValue) {
	EXPECT_TRUE(graph::StateRegistry::getBool("bool_key_true", false));
	EXPECT_FALSE(graph::StateRegistry::getBool("bool_key_false", true));
	graph::StateRegistry::setString("bool_yes", "yes");
	EXPECT_TRUE(graph::StateRegistry::getBool("bool_yes", false));
	graph::StateRegistry::setString("bool_no", "no");
	EXPECT_FALSE(graph::StateRegistry::getBool("bool_no", true));
	EXPECT_TRUE(graph::StateRegistry::getBool("not_exist", true));
}

TEST_F(StateRegistryTest, SetAndExistsAndRemove) {
	graph::StateRegistry::setString("new_key", "new_value");
	EXPECT_TRUE(graph::StateRegistry::exists("new_key"));
	EXPECT_EQ(graph::StateRegistry::getString("new_key", ""), "new_value");
	EXPECT_TRUE(graph::StateRegistry::remove("new_key"));
	EXPECT_FALSE(graph::StateRegistry::exists("new_key"));
	EXPECT_FALSE(graph::StateRegistry::remove("not_exist"));
}

TEST_F(StateRegistryTest, SetIntDoubleBool) {
	graph::StateRegistry::setInt("int2", 100);
	EXPECT_EQ(graph::StateRegistry::getInt("int2", 0), 100);
	graph::StateRegistry::setDouble("double2", 2.718);
	EXPECT_DOUBLE_EQ(graph::StateRegistry::getDouble("double2", 0.0), 2.718);
	graph::StateRegistry::setBool("bool2", true);
	EXPECT_TRUE(graph::StateRegistry::getBool("bool2", false));
}

TEST_F(StateRegistryTest, LoadAllStatesSkipsInactive) {
	// inactive_key should not be loaded
	EXPECT_EQ(graph::StateRegistry::getString("inactive_key", "notfound"), "notfound");
}

TEST_F(StateRegistryTest, ThreadSafety) {
	// Simple concurrency test
	auto setFunc = []() {
		for (int i = 0; i < 100; ++i) {
			graph::StateRegistry::setInt("thread_key", i);
		}
	};
	std::thread t1(setFunc), t2(setFunc);
	t1.join();
	t2.join();
	// The final value should be between 0 and 99
	int64_t val = graph::StateRegistry::getInt("thread_key", -1);
	EXPECT_GE(val, 0);
	EXPECT_LE(val, 99);
}

TEST_F(StateRegistryTest, InsertWithoutInit) {
	// Insert data before initialization
	graph::State preInitState(20, "pre_init_key_only", "pre_value_only");
	dataManager->addStateEntity(preInitState);
	// Do not call initialize here    // Do not call initialize here
	// Should not be able to read the value
	EXPECT_EQ(graph::StateRegistry::getString("pre_init_key_only", "notfound"), "pre_value_only");
}

TEST_F(StateRegistryTest, DeleteWithoutInit) {
	graph::State inactiveState(30, "delete_key_only", "delete_value_only");
	// Insert data before initialization
	dataManager->addStateEntity(inactiveState);
	// Delete the data before initialization
	dataManager->deleteState(inactiveState);
	// Do not call initialize here
	// Should not be able to read the value
	EXPECT_EQ(graph::StateRegistry::getString("delete_key_only", "notfound"), "notfound");
}
