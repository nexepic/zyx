/**
 * @file test_DirtyEntityRegistry.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/2
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "graph/core/Node.hpp"
#include "graph/storage/DirtyEntityRegistry.hpp"

using namespace graph::storage;
using namespace graph;

class DirtyEntityRegistryTest : public ::testing::Test {
protected:
	// We use Node as the template argument for testing generic logic
	DirtyEntityRegistry<Node> registry;

	static Node createNode(int64_t id, const std::string &label) {
		Node node;
		node.setId(id);
		node.setLabel(label);
		return node;
	}

	static DirtyEntityInfo<Node> createInfo(int64_t id, EntityChangeType type, const std::string &label = "") {
		Node node = createNode(id, label);
		return {type, node};
	}
};

// 1. Basic Upsert and Get (Active Layer)
TEST_F(DirtyEntityRegistryTest, BasicUpsertAndGet) {
	auto info = createInfo(1, EntityChangeType::ADDED, "Node1");
	registry.upsert(info);

	EXPECT_TRUE(registry.contains(1));
	EXPECT_EQ(registry.size(), 1UL);

	auto dirtyInfo = registry.getInfo(1);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(dirtyInfo->changeType, EntityChangeType::ADDED);

	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabel(), "Node1");
}

// 2. Snapshot Creation (Moving Active to Flushing)
TEST_F(DirtyEntityRegistryTest, SnapshotTransition) {
	registry.upsert(createInfo(1, EntityChangeType::ADDED, "Node1"));
	registry.createFlushSnapshot();

	EXPECT_TRUE(registry.contains(1));

	auto dirtyInfo = registry.getInfo(1);
	ASSERT_TRUE(dirtyInfo.has_value());
	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabel(), "Node1");
}

// 3. Double Buffering: Write to Active while Flushing exists
TEST_F(DirtyEntityRegistryTest, DoubleBufferingIsolation) {
	registry.upsert(createInfo(1, EntityChangeType::ADDED, "FlushNode"));
	registry.createFlushSnapshot();
	registry.upsert(createInfo(2, EntityChangeType::ADDED, "ActiveNode"));

	EXPECT_EQ(registry.size(), 2UL);

	auto info1 = registry.getInfo(1);
	ASSERT_TRUE(info1.has_value() && info1->backup.has_value());
	EXPECT_EQ(info1->backup->getLabel(), "FlushNode");

	auto info2 = registry.getInfo(2);
	ASSERT_TRUE(info2.has_value() && info2->backup.has_value());
	EXPECT_EQ(info2->backup->getLabel(), "ActiveNode");

	// 4. Verify Snapshot Content (Should only have FlushNode)
	// Note: We can't access private maps directly, but createFlushSnapshot returned the map earlier.
	// Let's create a NEW snapshot. This effectively merges the current Active into the Flushing?
	// According to logic: flushingMap_ = std::move(activeMap_) if flushing was empty.
	// But here flushing is NOT empty.

	// Let's test the merging logic or commit logic.
	// Commit the first flush
	registry.commitFlush();

	// Now only ActiveNode (ID 2) should remain
	EXPECT_EQ(registry.size(), 1UL);
	EXPECT_FALSE(registry.contains(1));
	EXPECT_TRUE(registry.contains(2));
}

// 4. Overwrite Logic: Active Update overrides Flushing Data
TEST_F(DirtyEntityRegistryTest, ActiveOverridesFlushing) {
	int64_t id = 1;
	registry.upsert(createInfo(id, EntityChangeType::ADDED, "Version1"));
	registry.createFlushSnapshot();
	registry.upsert(createInfo(id, EntityChangeType::MODIFIED, "Version2"));

	auto dirtyInfo = registry.getInfo(id);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(dirtyInfo->changeType, EntityChangeType::MODIFIED);

	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabel(), "Version2");
}

// 5. Query View: GetAllDirtyInfos (Merging Active + Flushing)
TEST_F(DirtyEntityRegistryTest, GetAllDirtyInfosMerge) {
	// Flushing: ID 1, ID 2
	registry.upsert(createInfo(1, EntityChangeType::ADDED, "Node1_v1"));
	registry.upsert(createInfo(2, EntityChangeType::ADDED, "Node2"));
	registry.createFlushSnapshot();

	// Active: ID 1 (Updated), ID 3
	registry.upsert(createInfo(1, EntityChangeType::MODIFIED, "Node1_v2"));
	registry.upsert(createInfo(3, EntityChangeType::ADDED, "Node3"));

	auto allInfos = registry.getAllDirtyInfos();

	// Should have 3 unique entities
	EXPECT_EQ(allInfos.size(), 3UL);

	bool found1 = false, found2 = false, found3 = false;
	for (const auto &info: allInfos) {
		int64_t id = info.backup->getId();
		if (id == 1) {
			found1 = true;
			EXPECT_EQ(info.backup->getLabel(), "Node1_v2") << "Should see the Active version";
			EXPECT_EQ(info.changeType, EntityChangeType::MODIFIED);
		} else if (id == 2) {
			found2 = true;
			EXPECT_EQ(info.backup->getLabel(), "Node2");
		} else if (id == 3) {
			found3 = true;
			EXPECT_EQ(info.backup->getLabel(), "Node3");
		}
	}
	EXPECT_TRUE(found1 && found2 && found3);
}

// 6. Delete Logic
TEST_F(DirtyEntityRegistryTest, DeleteMarker) {
	// Upsert a DELETED info
	registry.upsert(createInfo(1, EntityChangeType::DELETED));

	// It should exist in the registry
	EXPECT_TRUE(registry.contains(1));

	// getEntity should return the backup (which might be empty or inactive depending on how it was passed)
	// The registry just stores what it's given.
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::DELETED);
}

// 7. Clear
TEST_F(DirtyEntityRegistryTest, Clear) {
	registry.upsert(createInfo(1, EntityChangeType::ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createInfo(2, EntityChangeType::ADDED));

	EXPECT_EQ(registry.size(), 2UL);

	registry.clear();

	EXPECT_EQ(registry.size(), 0UL);
	EXPECT_FALSE(registry.contains(1));
	EXPECT_FALSE(registry.contains(2));
}
