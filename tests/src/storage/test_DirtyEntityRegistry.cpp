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

// 8. Remove from active map
TEST_F(DirtyEntityRegistryTest, RemoveFromActive) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10));
	EXPECT_TRUE(registry.contains(1));

	bool removed = registry.remove(1);
	EXPECT_TRUE(removed);
	EXPECT_FALSE(registry.contains(1));
	EXPECT_EQ(registry.size(), 0UL);
}

// 9. Remove blocked when entity is in flushing map
TEST_F(DirtyEntityRegistryTest, RemoveBlockedByFlushing) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10));
	registry.createFlushSnapshot(); // entity 1 now in flushing map

	bool removed = registry.remove(1);
	EXPECT_FALSE(removed); // Cannot remove: entity is being flushed
	EXPECT_TRUE(registry.contains(1));
}

// 10. Remove non-existent entity returns true (safely)
TEST_F(DirtyEntityRegistryTest, RemoveNonExistent) {
	bool removed = registry.remove(999);
	EXPECT_TRUE(removed);
	EXPECT_EQ(registry.size(), 0UL);
}

// 11. Upsert with no backup (backup is nullopt) should not add to registry
TEST_F(DirtyEntityRegistryTest, UpsertWithoutBackup) {
	DirtyEntityInfo<Node> info(EntityChangeType::CHANGE_MODIFIED);
	// info.backup is nullopt
	registry.upsert(info);
	EXPECT_EQ(registry.size(), 0UL);
}

// 12. createFlushSnapshot with non-empty flushing map merges active into flushing
TEST_F(DirtyEntityRegistryTest, SnapshotMergeIntoExistingFlushing) {
	// Create a flushing snapshot with entity 1
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10));
	registry.createFlushSnapshot();

	// Add entity 2 to active
	registry.upsert(createInfo(2, EntityChangeType::CHANGE_ADDED, 20));

	// Create another snapshot before committing the first one
	// This should merge active (entity 2) into flushing (entity 1)
	auto snapshot = registry.createFlushSnapshot();

	EXPECT_EQ(snapshot.size(), 2UL);
	EXPECT_TRUE(snapshot.contains(1));
	EXPECT_TRUE(snapshot.contains(2));
}

// 13. getAllDirtyInfos with empty registry
TEST_F(DirtyEntityRegistryTest, GetAllDirtyInfosEmpty) {
	auto all = registry.getAllDirtyInfos();
	EXPECT_TRUE(all.empty());
}

// 14. getInfo returns nullopt for non-existent
TEST_F(DirtyEntityRegistryTest, GetInfoNonExistent) {
	auto result = registry.getInfo(999);
	EXPECT_FALSE(result.has_value());
}

// 15. Contains checks both maps
TEST_F(DirtyEntityRegistryTest, ContainsBothMaps) {
	registry.upsert(createInfo(1, EntityChangeType::CHANGE_ADDED, 10));
	registry.createFlushSnapshot();
	registry.upsert(createInfo(2, EntityChangeType::CHANGE_ADDED, 20));

	// Entity 1 is in flushing map
	EXPECT_TRUE(registry.contains(1));
	// Entity 2 is in active map
	EXPECT_TRUE(registry.contains(2));
	// Entity 3 doesn't exist
	EXPECT_FALSE(registry.contains(3));
}

// =============================================================================
// Tests with additional entity types (Edge, Property, Blob, Index, State) to
// improve branch coverage for template instantiations.
// =============================================================================

#include "graph/core/Edge.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/State.hpp"

// --- Edge ---

class DirtyEntityRegistryEdgeTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Edge> registry;

	static DirtyEntityInfo<Edge> createEdgeInfo(int64_t id, EntityChangeType type) {
		Edge edge;
		edge.setId(id);
		edge.setSourceNodeId(id * 10);
		edge.setTargetNodeId(id * 20);
		return {type, edge};
	}
};

TEST_F(DirtyEntityRegistryEdgeTest, UpsertGetContains) {
	auto info = createEdgeInfo(1, EntityChangeType::CHANGE_ADDED);
	registry.upsert(info);

	EXPECT_TRUE(registry.contains(1));
	EXPECT_EQ(registry.size(), 1UL);

	auto retrieved = registry.getInfo(1);
	ASSERT_TRUE(retrieved.has_value());
	EXPECT_EQ(retrieved->changeType, EntityChangeType::CHANGE_ADDED);
	ASSERT_TRUE(retrieved->backup.has_value());
	EXPECT_EQ(retrieved->backup->getId(), 1);
	EXPECT_EQ(retrieved->backup->getSourceNodeId(), 10);
}

TEST_F(DirtyEntityRegistryEdgeTest, SnapshotAndCommitFlush) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.upsert(createEdgeInfo(2, EntityChangeType::CHANGE_MODIFIED));

	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);

	// Add new entity while flushing
	registry.upsert(createEdgeInfo(3, EntityChangeType::CHANGE_ADDED));
	EXPECT_EQ(registry.size(), 3UL);

	registry.commitFlush();
	EXPECT_EQ(registry.size(), 1UL);
	EXPECT_TRUE(registry.contains(3));
	EXPECT_FALSE(registry.contains(1));
}

TEST_F(DirtyEntityRegistryEdgeTest, GetAllDirtyInfosMerge) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_MODIFIED));
	registry.upsert(createEdgeInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);
}

TEST_F(DirtyEntityRegistryEdgeTest, RemoveActiveAndBlocked) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(registry.remove(1));
	EXPECT_FALSE(registry.contains(1));

	registry.upsert(createEdgeInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	EXPECT_FALSE(registry.remove(2)); // blocked by flushing
}

TEST_F(DirtyEntityRegistryEdgeTest, ClearAndUpsertWithoutBackup) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createEdgeInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.clear();
	EXPECT_EQ(registry.size(), 0UL);

	// Upsert without backup
	DirtyEntityInfo<Edge> noBackup(EntityChangeType::CHANGE_MODIFIED);
	registry.upsert(noBackup);
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryEdgeTest, SnapshotMergeIntoExistingFlushing) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createEdgeInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TEST_F(DirtyEntityRegistryEdgeTest, GetInfoFromFlushingMap) {
	registry.upsert(createEdgeInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();

	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TEST_F(DirtyEntityRegistryEdgeTest, GetInfoNonExistent) {
	EXPECT_FALSE(registry.getInfo(999).has_value());
}

// --- Property ---

class DirtyEntityRegistryPropertyTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Property> registry;

	static DirtyEntityInfo<Property> createPropInfo(int64_t id, EntityChangeType type) {
		Property prop(id, id * 100, 0);
		return {type, prop};
	}
};

TEST_F(DirtyEntityRegistryPropertyTest, UpsertGetContains) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));

	EXPECT_TRUE(registry.contains(1));
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TEST_F(DirtyEntityRegistryPropertyTest, SnapshotAndCommitFlush) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 1UL);

	registry.upsert(createPropInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.commitFlush();
	EXPECT_EQ(registry.size(), 1UL);
	EXPECT_TRUE(registry.contains(2));
}

TEST_F(DirtyEntityRegistryPropertyTest, GetAllDirtyInfosMerge) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_MODIFIED));
	registry.upsert(createPropInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);
}

TEST_F(DirtyEntityRegistryPropertyTest, RemoveAndClear) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(registry.remove(1));

	registry.upsert(createPropInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	EXPECT_FALSE(registry.remove(2));

	registry.clear();
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryPropertyTest, UpsertWithoutBackup) {
	DirtyEntityInfo<Property> noBackup(EntityChangeType::CHANGE_MODIFIED);
	registry.upsert(noBackup);
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryPropertyTest, SnapshotMergeIntoExistingFlushing) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createPropInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TEST_F(DirtyEntityRegistryPropertyTest, GetInfoFromFlushingMap) {
	registry.upsert(createPropInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
}

// --- Blob ---

class DirtyEntityRegistryBlobTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Blob> registry;

	static DirtyEntityInfo<Blob> createBlobInfo(int64_t id, EntityChangeType type) {
		Blob blob;
		blob.setId(id);
		return {type, blob};
	}
};

TEST_F(DirtyEntityRegistryBlobTest, UpsertGetContains) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));

	EXPECT_TRUE(registry.contains(1));
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TEST_F(DirtyEntityRegistryBlobTest, SnapshotAndCommitFlush) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 1UL);

	registry.upsert(createBlobInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.commitFlush();
	EXPECT_EQ(registry.size(), 1UL);
}

TEST_F(DirtyEntityRegistryBlobTest, GetAllDirtyInfosMerge) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_MODIFIED));
	registry.upsert(createBlobInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);
}

TEST_F(DirtyEntityRegistryBlobTest, RemoveAndClear) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(registry.remove(1));

	registry.upsert(createBlobInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	EXPECT_FALSE(registry.remove(2));

	registry.clear();
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryBlobTest, UpsertWithoutBackup) {
	DirtyEntityInfo<Blob> noBackup(EntityChangeType::CHANGE_MODIFIED);
	registry.upsert(noBackup);
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryBlobTest, SnapshotMergeIntoExistingFlushing) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createBlobInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TEST_F(DirtyEntityRegistryBlobTest, GetInfoFromFlushingMap) {
	registry.upsert(createBlobInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
}

TEST_F(DirtyEntityRegistryBlobTest, GetInfoNonExistent) {
	EXPECT_FALSE(registry.getInfo(999).has_value());
}

// --- Index ---

class DirtyEntityRegistryIndexTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Index> registry;

	static DirtyEntityInfo<Index> createIndexInfo(int64_t id, EntityChangeType type) {
		Index idx;
		idx.setId(id);
		return {type, idx};
	}
};

TEST_F(DirtyEntityRegistryIndexTest, UpsertGetContains) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));

	EXPECT_TRUE(registry.contains(1));
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TEST_F(DirtyEntityRegistryIndexTest, SnapshotAndCommitFlush) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 1UL);

	registry.upsert(createIndexInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.commitFlush();
	EXPECT_EQ(registry.size(), 1UL);
}

TEST_F(DirtyEntityRegistryIndexTest, GetAllDirtyInfosMerge) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_MODIFIED));
	registry.upsert(createIndexInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);
}

TEST_F(DirtyEntityRegistryIndexTest, RemoveAndClear) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(registry.remove(1));

	registry.upsert(createIndexInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	EXPECT_FALSE(registry.remove(2));

	registry.clear();
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryIndexTest, UpsertWithoutBackup) {
	DirtyEntityInfo<Index> noBackup(EntityChangeType::CHANGE_MODIFIED);
	registry.upsert(noBackup);
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryIndexTest, SnapshotMergeIntoExistingFlushing) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createIndexInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TEST_F(DirtyEntityRegistryIndexTest, GetInfoFromFlushingMap) {
	registry.upsert(createIndexInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
}

TEST_F(DirtyEntityRegistryIndexTest, GetInfoNonExistent) {
	EXPECT_FALSE(registry.getInfo(999).has_value());
}

// --- State ---

class DirtyEntityRegistryStateTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<State> registry;

	static DirtyEntityInfo<State> createStateInfo(int64_t id, EntityChangeType type) {
		State state;
		state.setId(id);
		state.setKey("key_" + std::to_string(id));
		return {type, state};
	}
};

TEST_F(DirtyEntityRegistryStateTest, UpsertGetContains) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));

	EXPECT_TRUE(registry.contains(1));
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
	EXPECT_EQ(info->backup->getKey(), "key_1");
}

TEST_F(DirtyEntityRegistryStateTest, SnapshotAndCommitFlush) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 1UL);

	registry.upsert(createStateInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.commitFlush();
	EXPECT_EQ(registry.size(), 1UL);
}

TEST_F(DirtyEntityRegistryStateTest, GetAllDirtyInfosMerge) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_MODIFIED));
	registry.upsert(createStateInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);
}

TEST_F(DirtyEntityRegistryStateTest, RemoveAndClear) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(registry.remove(1));

	registry.upsert(createStateInfo(2, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	EXPECT_FALSE(registry.remove(2));

	registry.clear();
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryStateTest, UpsertWithoutBackup) {
	DirtyEntityInfo<State> noBackup(EntityChangeType::CHANGE_MODIFIED);
	registry.upsert(noBackup);
	EXPECT_EQ(registry.size(), 0UL);
}

TEST_F(DirtyEntityRegistryStateTest, SnapshotMergeIntoExistingFlushing) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	registry.upsert(createStateInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TEST_F(DirtyEntityRegistryStateTest, GetInfoFromFlushingMap) {
	registry.upsert(createStateInfo(1, EntityChangeType::CHANGE_ADDED));
	registry.createFlushSnapshot();
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
}

TEST_F(DirtyEntityRegistryStateTest, GetInfoNonExistent) {
	EXPECT_FALSE(registry.getInfo(999).has_value());
}

// =============================================================================
// Typed tests: Instantiate DirtyEntityRegistry with all entity types
// =============================================================================

template<typename T>
class DirtyEntityRegistryTypedTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<T> registry;

	static DirtyEntityInfo<T> makeInfo(int64_t id, EntityChangeType type) {
		T entity;
		entity.setId(id);
		return {type, entity};
	}
};

using AllEntityTypes = ::testing::Types<Node, Edge, Property, Blob, Index, State>;
TYPED_TEST_SUITE(DirtyEntityRegistryTypedTest, AllEntityTypes);

TYPED_TEST(DirtyEntityRegistryTypedTest, UpsertAndContains) {
	auto info = this->makeInfo(1, EntityChangeType::CHANGE_ADDED);
	this->registry.upsert(info);

	EXPECT_TRUE(this->registry.contains(1));
	EXPECT_EQ(this->registry.size(), 1UL);
}

TYPED_TEST(DirtyEntityRegistryTypedTest, GetInfoFromActive) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));

	auto info = this->registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::CHANGE_ADDED);
	ASSERT_TRUE(info->backup.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TYPED_TEST(DirtyEntityRegistryTypedTest, GetInfoFromFlushing) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();

	auto info = this->registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getId(), 1);
}

TYPED_TEST(DirtyEntityRegistryTypedTest, GetInfoNonExistent) {
	EXPECT_FALSE(this->registry.getInfo(999).has_value());
}

TYPED_TEST(DirtyEntityRegistryTypedTest, RemoveFromActive) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	EXPECT_TRUE(this->registry.remove(1));
	EXPECT_FALSE(this->registry.contains(1));
}

TYPED_TEST(DirtyEntityRegistryTypedTest, RemoveBlockedByFlushing) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	EXPECT_FALSE(this->registry.remove(1));
	EXPECT_TRUE(this->registry.contains(1));
}

TYPED_TEST(DirtyEntityRegistryTypedTest, SnapshotAndCommit) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.upsert(this->makeInfo(2, EntityChangeType::CHANGE_MODIFIED));

	auto snapshot = this->registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);

	this->registry.upsert(this->makeInfo(3, EntityChangeType::CHANGE_ADDED));
	EXPECT_EQ(this->registry.size(), 3UL);

	this->registry.commitFlush();
	EXPECT_EQ(this->registry.size(), 1UL);
	EXPECT_TRUE(this->registry.contains(3));
}

TYPED_TEST(DirtyEntityRegistryTypedTest, SnapshotMerge) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	this->registry.upsert(this->makeInfo(2, EntityChangeType::CHANGE_ADDED));
	auto snapshot = this->registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.size(), 2UL);
}

TYPED_TEST(DirtyEntityRegistryTypedTest, GetAllDirtyInfosMerge) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_MODIFIED));
	this->registry.upsert(this->makeInfo(2, EntityChangeType::CHANGE_ADDED));

	auto all = this->registry.getAllDirtyInfos();
	EXPECT_EQ(all.size(), 2UL);

	// Verify active version of entity 1 takes precedence
	for (const auto &info : all) {
		if (info.backup->getId() == 1) {
			EXPECT_EQ(info.changeType, EntityChangeType::CHANGE_MODIFIED);
		}
	}
}

TYPED_TEST(DirtyEntityRegistryTypedTest, Clear) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	this->registry.upsert(this->makeInfo(2, EntityChangeType::CHANGE_ADDED));

	this->registry.clear();
	EXPECT_EQ(this->registry.size(), 0UL);
	EXPECT_FALSE(this->registry.contains(1));
	EXPECT_FALSE(this->registry.contains(2));
}

TYPED_TEST(DirtyEntityRegistryTypedTest, UpsertWithoutBackup) {
	DirtyEntityInfo<TypeParam> noBackup(EntityChangeType::CHANGE_MODIFIED);
	this->registry.upsert(noBackup);
	EXPECT_EQ(this->registry.size(), 0UL);
}

TYPED_TEST(DirtyEntityRegistryTypedTest, ContainsBothMaps) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	this->registry.upsert(this->makeInfo(2, EntityChangeType::CHANGE_ADDED));

	EXPECT_TRUE(this->registry.contains(1));  // flushing
	EXPECT_TRUE(this->registry.contains(2));  // active
	EXPECT_FALSE(this->registry.contains(3)); // neither
}

TYPED_TEST(DirtyEntityRegistryTypedTest, ActiveOverridesFlushing) {
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_ADDED));
	this->registry.createFlushSnapshot();
	this->registry.upsert(this->makeInfo(1, EntityChangeType::CHANGE_DELETED));

	auto info = this->registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::CHANGE_DELETED);
}
