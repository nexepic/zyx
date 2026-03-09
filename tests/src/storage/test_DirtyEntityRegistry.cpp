/**
 * @file test_DirtyEntityRegistry.cpp
 * @author Nexepic
 * @date 2025/12/2
 *
 * @copyright Copyright (c) 2025 Nexepic
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

	static Node createNode(int64_t id, int64_t labelId) {
		Node node;
		node.setId(id);
		node.setLabelId(labelId);
		return node;
	}

	static DirtyEntityInfo<Node> createInfo(int64_t id, EntityChangeType type, int64_t labelId = 0) {
		Node node = createNode(id, labelId);
		return {type, node};
	}
};

// 1. Basic Upsert and Get (Active Layer)
TEST_F(DirtyEntityRegistryTest, BasicUpsertAndGet) {
	auto info = createInfo(1, EntityChangeType::CHANGE_ADDED, 10);
	registry.upsert(info);

	EXPECT_TRUE(registry.contains(1));
	EXPECT_EQ(registry.size(), 1UL);

	auto dirtyInfo = registry.getInfo(1);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(dirtyInfo->changeType, EntityChangeType::CHANGE_ADDED);

	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabelId(), 10);
}

// 2. Snapshot Creation (Moving Active to Flushing)
TEST_F(DirtyEntityRegistryTest, SnapshotTransition) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10));
	registry.createFlushSnapshot();

	EXPECT_TRUE(registry.contains(1));

	auto dirtyInfo = registry.getInfo(1);
	ASSERT_TRUE(dirtyInfo.has_value());
	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabelId(), 10);
}

// 3. Double Buffering: Write to Active while Flushing exists
TEST_F(DirtyEntityRegistryTest, DoubleBufferingIsolation) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10)); // FlushNode
	registry.createFlushSnapshot();
	registry.upsert(createInfo(2, EntityChangeType::CHANGE_ADDED, 20)); // ActiveNode

	EXPECT_EQ(registry.size(), 2UL);

	auto info1 = registry.getInfo(1);
	ASSERT_TRUE(info1.has_value() && info1->backup.has_value());
	EXPECT_EQ(info1->backup->getLabelId(), 10);

	auto info2 = registry.getInfo(2);
	ASSERT_TRUE(info2.has_value() && info2->backup.has_value());
	EXPECT_EQ(info2->backup->getLabelId(), 20);

	registry.commitFlush();

	EXPECT_EQ(registry.size(), 1UL);
	EXPECT_FALSE(registry.contains(1));
	EXPECT_TRUE(registry.contains(2));
}

// 4. Overwrite Logic: Active Update overrides Flushing Data
TEST_F(DirtyEntityRegistryTest, ActiveOverridesFlushing) {
	int64_t id = 1;
	registry.upsert(createInfo(id, EntityChangeType::CHANGE_ADDED, 10)); // v1
	registry.createFlushSnapshot();
	registry.upsert(createInfo(id, EntityChangeType::CHANGE_MODIFIED, 20)); // v2

	auto dirtyInfo = registry.getInfo(id);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(dirtyInfo->changeType, EntityChangeType::CHANGE_MODIFIED);

	ASSERT_TRUE(dirtyInfo->backup.has_value());
	EXPECT_EQ(dirtyInfo->backup->getLabelId(), 20); // Check v2 ID
}

// 5. Query View: GetAllDirtyInfos (Merging Active + Flushing)
TEST_F(DirtyEntityRegistryTest, GetAllDirtyInfosMerge) {
	// Flushing: ID 1, ID 2
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10)); // v1
	registry.upsert(createInfo(2, EntityChangeType::CHANGE_ADDED, 20));
	registry.createFlushSnapshot();

	// Active: ID 1 (Updated), ID 3
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_MODIFIED, 11)); // v2
	registry.upsert(createInfo(3, EntityChangeType::CHANGE_ADDED, 30));

	auto allInfos = registry.getAllDirtyInfos();

	EXPECT_EQ(allInfos.size(), 3UL);

	bool found1 = false, found2 = false, found3 = false;
	for (const auto &info: allInfos) {
		int64_t id = info.backup->getId();
		if (id == 1) {
			found1 = true;
			EXPECT_EQ(info.backup->getLabelId(), 11) << "Should see the Active version";
			EXPECT_EQ(info.changeType, EntityChangeType::CHANGE_MODIFIED);
		} else if (id == 2) {
			found2 = true;
			EXPECT_EQ(info.backup->getLabelId(), 20);
		} else if (id == 3) {
			found3 = true;
			EXPECT_EQ(info.backup->getLabelId(), 30);
		}
	}
	EXPECT_TRUE(found1 && found2 && found3);
}

// 6. Delete Logic
TEST_F(DirtyEntityRegistryTest, DeleteMarker) {
	// Upsert a DELETED info
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_DELETED));

	// It should exist in the registry
	EXPECT_TRUE(registry.contains(1));

	// getEntity should return the backup (which might be empty or inactive depending on how it was passed)
	// The registry just stores what it's given.
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::CHANGE_DELETED);
}

// 7. Clear
TEST_F(DirtyEntityRegistryTest, Clear) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createInfo(2, EntityChangeType::CHANGE_ADDED));

	EXPECT_EQ(registry.size(), 2UL);

	registry.clear();

	EXPECT_EQ(registry.size(), 0UL);
	EXPECT_FALSE(registry.contains(1));
	EXPECT_FALSE(registry.contains(2));
}
