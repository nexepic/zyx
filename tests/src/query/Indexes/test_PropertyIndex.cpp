/**
 * @file test_PropertyIndex.cpp
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
#include "graph/core/Database.hpp"
#include "graph/query/indexes/PropertyIndex.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class PropertyIndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_propertyIndex_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		propertyIndex = std::make_unique<graph::query::indexes::PropertyIndex>(dataManager);
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
	std::unique_ptr<graph::query::indexes::PropertyIndex> propertyIndex;
};

TEST_F(PropertyIndexTest, IsEmptyInitially) { EXPECT_TRUE(propertyIndex->isEmpty()); }

TEST_F(PropertyIndexTest, AddAndFindStringProperty) {
	int64_t nodeId = 1;
	std::string key = "name";
	std::string value = "Alice";
	propertyIndex->addProperty(nodeId, key, value);
	auto found = propertyIndex->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
	EXPECT_FALSE(propertyIndex->isEmpty());
}

TEST_F(PropertyIndexTest, AddAndFindIntProperty) {
	int64_t nodeId = 2;
	std::string key = "age";
	int64_t value = 30;
	propertyIndex->addProperty(nodeId, key, value);
	auto found = propertyIndex->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, AddAndFindDoubleProperty) {
	int64_t nodeId = 3;
	std::string key = "score";
	double value = 99.5;
	propertyIndex->addProperty(nodeId, key, value);
	auto found = propertyIndex->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, AddAndFindBoolProperty) {
	int64_t nodeId = 4;
	std::string key = "active";
	bool value = true;
	propertyIndex->addProperty(nodeId, key, value);
	auto found = propertyIndex->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, HasPropertyValue) {
	int64_t nodeId = 5;
	std::string key = "flag";
	bool value = false;
	propertyIndex->addProperty(nodeId, key, value);
	EXPECT_TRUE(propertyIndex->hasPropertyValue(nodeId, key, value));
	EXPECT_FALSE(propertyIndex->hasPropertyValue(nodeId, key, true));
}

TEST_F(PropertyIndexTest, RemoveProperty) {
	int64_t nodeId = 6;
	std::string key = "remove";
	std::string value = "test";
	propertyIndex->addProperty(nodeId, key, value);
	EXPECT_TRUE(propertyIndex->hasPropertyValue(nodeId, key, value));
	propertyIndex->removeProperty(nodeId, key);
	EXPECT_FALSE(propertyIndex->hasPropertyValue(nodeId, key, value));
}

TEST_F(PropertyIndexTest, FindRange) {
	int64_t nodeId1 = 7, nodeId2 = 8, nodeId3 = 9;
	std::string key = "range";
	int64_t v1 = 10, v2 = 20, v3 = 30;
	propertyIndex->addProperty(nodeId1, key, v1);
	propertyIndex->addProperty(nodeId2, key, v2);
	propertyIndex->addProperty(nodeId3, key, v3);
	auto found = propertyIndex->findRange(key, 15, 30);
	EXPECT_EQ(found.size(), 2);
	EXPECT_TRUE(std::find(found.begin(), found.end(), nodeId2) != found.end());
	EXPECT_TRUE(std::find(found.begin(), found.end(), nodeId3) != found.end());
}

TEST_F(PropertyIndexTest, GetIndexedKeys) {
	propertyIndex->addProperty(10, "k1", "v1");
	propertyIndex->addProperty(11, "k2", 123);
	propertyIndex->addProperty(12, "k3", 1.23);
	propertyIndex->addProperty(13, "k4", true);
	auto keys = propertyIndex->getIndexedKeys();
	EXPECT_EQ(keys.size(), 4);
	EXPECT_TRUE(std::find(keys.begin(), keys.end(), "k1") != keys.end());
	EXPECT_TRUE(std::find(keys.begin(), keys.end(), "k2") != keys.end());
	EXPECT_TRUE(std::find(keys.begin(), keys.end(), "k3") != keys.end());
	EXPECT_TRUE(std::find(keys.begin(), keys.end(), "k4") != keys.end());
}

TEST_F(PropertyIndexTest, ClearAndDrop) {
	propertyIndex->addProperty(20, "clear", "v");
	propertyIndex->addProperty(21, "clear", 1);
	EXPECT_FALSE(propertyIndex->isEmpty());
	propertyIndex->clear();
	EXPECT_TRUE(propertyIndex->isEmpty());
	// Drop should not throw
	EXPECT_NO_THROW(propertyIndex->drop());
}
