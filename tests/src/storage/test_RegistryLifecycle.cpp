/**
 * @file test_RegistryLifecycle.cpp
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
#include "graph/core/Node.hpp"
#include "graph/storage/DirtyEntityRegistry.hpp"

using namespace graph::storage;
using namespace graph;

class RegistryLifecycleTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Node> registry;

	static DirtyEntityInfo<Node> makeInfo(int64_t id, EntityChangeType type, const std::string &label = "") {
		Node n;
		n.setId(id);
		n.setLabel(label);
		return {type, n};
	}
};

// SCENARIO: Update Active -> Snapshot -> Update Again (Active) -> Read
// Verifies that the new update in Active properly masks the old version in Flushing.
TEST_F(RegistryLifecycleTest, UpdateWhileFlushing) {
	// 1. Initial Add
	registry.upsert(makeInfo(1, EntityChangeType::ADDED, "v1"));

	// 2. Snapshot (v1 moves to Flushing)
	auto snapshot = registry.createFlushSnapshot();
	EXPECT_EQ(snapshot.at(1).backup->getLabel(), "v1");

	// 3. Update while flushing (v2 goes to Active)
	registry.upsert(makeInfo(1, EntityChangeType::MODIFIED, "v2"));

	// 4. Verify getInfo returns v2 (Priority: Active > Flushing)
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getLabel(), "v2");
	EXPECT_EQ(info->changeType, EntityChangeType::MODIFIED);

	// 5. Commit Flush (v1 removed from Flushing, v2 stays in Active)
	registry.commitFlush();

	// 6. Verify v2 still exists
	info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getLabel(), "v2");
}

// SCENARIO: Add -> Snapshot -> Delete -> Read
// Verifies that a delete operation in Active correctly hides an entity currently being flushed.
TEST_F(RegistryLifecycleTest, DeleteWhileFlushing) {
	// 1. Add v1 and Snapshot
	registry.upsert(makeInfo(1, EntityChangeType::ADDED, "v1"));
	registry.createFlushSnapshot();

	// 2. Delete (Active gets DELETED marker)
	registry.upsert(makeInfo(1, EntityChangeType::DELETED));

	// 3. Verify getInfo returns DELETED
	auto info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::DELETED);

	// 4. Commit Flush (v1 written to disk)
	// Note: In a real system, PersistenceManager::commitFlush doesn't delete from disk.
	// The FileStorage::save() logic handles the disk deletion based on the snapshot.
	// Here we just test Registry state.
	registry.commitFlush();

	// 5. Registry should still hold the DELETED marker in Active
	// Because the delete happened *after* the snapshot, it wasn't part of the commit.
	// It is waiting for the NEXT flush.
	info = registry.getInfo(1);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->changeType, EntityChangeType::DELETED);
}

// SCENARIO: Empty Flush
// Verifies system stability when flushing nothing.
TEST_F(RegistryLifecycleTest, EmptyFlushCycle) {
	registry.createFlushSnapshot(); // Snapshot empty
	registry.commitFlush(); // Commit empty

	registry.upsert(makeInfo(1, EntityChangeType::ADDED));
	EXPECT_EQ(registry.size(), 1UL);
}
