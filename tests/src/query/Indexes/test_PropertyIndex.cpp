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
	// SetUpTestSuite is run ONCE for the entire test suite.
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath_ = std::filesystem::temp_directory_path() / ("test_propertyIndex_" + boost::uuids::to_string(uuid) + ".dat");

        database_ = std::make_unique<graph::Database>(testFilePath_.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

    // TearDownTestSuite is run ONCE after all tests in the suite have finished.
    static void TearDownTestSuite() {
        if (database_) {
            database_->close();
            database_.reset(); // This will trigger database destructor and file cleanup if applicable.
        }
		// In case the DB doesn't clean up, we do it manually.
        if (std::filesystem::exists(testFilePath_)) {
            std::filesystem::remove(testFilePath_);
        }
    }

	// SetUp is run before EACH TEST_F.
	void SetUp() override {
        // Create a fresh, empty PropertyIndex for each test to ensure isolation.
		propertyIndex_ = std::make_unique<graph::query::indexes::PropertyIndex>(dataManager_);
	}

    // TearDown is run after EACH TEST_F.
    void TearDown() override {
        propertyIndex_->clear(); // Clean up any created indexes
    }

    // Static members are shared across all tests in the suite.
	static inline std::filesystem::path testFilePath_;
	static inline std::unique_ptr<graph::Database> database_;
	static inline std::shared_ptr<graph::storage::DataManager> dataManager_;

    // Non-static members are unique for each test.
	std::unique_ptr<graph::query::indexes::PropertyIndex> propertyIndex_;
};

TEST_F(PropertyIndexTest, IsEmptyInitially) {
    EXPECT_TRUE(propertyIndex_->isEmpty());
}

TEST_F(PropertyIndexTest, AddAndFindStringProperty) {
	int64_t nodeId = 1;
	std::string key = "name";
	std::string value = "Alice";
	propertyIndex_->addProperty(nodeId, key, value);

	auto found = propertyIndex_->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
	EXPECT_FALSE(propertyIndex_->isEmpty());
}

TEST_F(PropertyIndexTest, AddAndFindIntProperty) {
	int64_t nodeId = 2;
	std::string key = "age";
	int64_t value = 30;
	propertyIndex_->addProperty(nodeId, key, value);

	auto found = propertyIndex_->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, AddAndFindDoubleProperty) {
	int64_t nodeId = 3;
	std::string key = "score";
	double value = 99.5;
	propertyIndex_->addProperty(nodeId, key, value);

	auto found = propertyIndex_->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, AddAndFindBoolProperty) {
	int64_t nodeId = 4;
	std::string key = "active";
	bool value = true;
	propertyIndex_->addProperty(nodeId, key, value);

	auto found = propertyIndex_->findExactMatch(key, value);
	ASSERT_EQ(found.size(), 1);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(PropertyIndexTest, HasPropertyValue) {
	int64_t nodeId = 5;
	std::string key = "flag";
	bool value = false;
	propertyIndex_->addProperty(nodeId, key, value);

	EXPECT_TRUE(propertyIndex_->hasPropertyValue(nodeId, key, value));
	EXPECT_FALSE(propertyIndex_->hasPropertyValue(nodeId + 1, key, value)); // Different node
	EXPECT_FALSE(propertyIndex_->hasPropertyValue(nodeId, key, true));      // Different value
}

TEST_F(PropertyIndexTest, RemoveProperty) {
	int64_t nodeId = 6;
	std::string key = "status";
	std::string value = "pending";
	propertyIndex_->addProperty(nodeId, key, value);
	ASSERT_TRUE(propertyIndex_->hasPropertyValue(nodeId, key, value));

	// CORRECTED: Call removeProperty with the value that was indexed.
	propertyIndex_->removeProperty(nodeId, key, value);

    EXPECT_FALSE(propertyIndex_->hasPropertyValue(nodeId, key, value));
}

TEST_F(PropertyIndexTest, UpdateProperty) {
    // This test verifies the "remove old, add new" logic for updates.
    int64_t nodeId = 7;
    std::string key = "status";
    std::string oldValue = "pending";
    std::string newValue = "approved";

    // 1. Add the initial value
    propertyIndex_->addProperty(nodeId, key, oldValue);
    ASSERT_TRUE(propertyIndex_->hasPropertyValue(nodeId, key, oldValue));
    ASSERT_FALSE(propertyIndex_->hasPropertyValue(nodeId, key, newValue));

    // 2. Simulate an update by removing the old and adding the new
    propertyIndex_->removeProperty(nodeId, key, oldValue);
    propertyIndex_->addProperty(nodeId, key, newValue);

    // 3. Verify the state is correct
    ASSERT_FALSE(propertyIndex_->hasPropertyValue(nodeId, key, oldValue));
    ASSERT_TRUE(propertyIndex_->hasPropertyValue(nodeId, key, newValue));
}

TEST_F(PropertyIndexTest, FindRange) {
	int64_t nodeId1 = 8, nodeId2 = 9, nodeId3 = 10;
	std::string key = "level";
	propertyIndex_->addProperty(nodeId1, key, static_cast<int64_t>(10));
	propertyIndex_->addProperty(nodeId2, key, static_cast<int64_t>(20));
	propertyIndex_->addProperty(nodeId3, key, 30.5); // Add a double too

	// Range query on integers [15, 25]
	auto foundInts = propertyIndex_->findRange(key, 15, 25);
	ASSERT_EQ(foundInts.size(), 1);
	EXPECT_EQ(foundInts[0], nodeId2);

    // Range query on doubles [20.0, 40.0]
    auto foundDoubles = propertyIndex_->findRange(key, 20.0, 40.0);
    // Should find the int 20 and the double 30.5
    ASSERT_EQ(foundDoubles.size(), 2);
    EXPECT_TRUE(std::ranges::find(foundDoubles, nodeId2) != foundDoubles.end());
    EXPECT_TRUE(std::ranges::find(foundDoubles, nodeId3) != foundDoubles.end());
}

TEST_F(PropertyIndexTest, GetIndexedKeys) {
	propertyIndex_->addProperty(11, "k1", "v1");
	propertyIndex_->addProperty(12, "k2", 123);
	propertyIndex_->addProperty(13, "k3", 1.23);
	propertyIndex_->addProperty(14, "k4", true);
    // Add another property with the same key but different type
    propertyIndex_->addProperty(15, "k1", 456);

	auto keys = propertyIndex_->getIndexedKeys();
    std::ranges::sort(keys);

	ASSERT_EQ(keys.size(), 4);
	EXPECT_EQ(keys[0], "k1");
    EXPECT_EQ(keys[1], "k2");
    EXPECT_EQ(keys[2], "k3");
    EXPECT_EQ(keys[3], "k4");
}

TEST_F(PropertyIndexTest, ClearAndDrop) {
	propertyIndex_->addProperty(20, "clear", "v");
	propertyIndex_->addProperty(21, "clear_int", 1);
	EXPECT_FALSE(propertyIndex_->isEmpty());

	propertyIndex_->clear();
	EXPECT_TRUE(propertyIndex_->isEmpty());

    // Drop should not throw and should leave the index empty
	EXPECT_NO_THROW(propertyIndex_->drop());
    EXPECT_TRUE(propertyIndex_->isEmpty());
}