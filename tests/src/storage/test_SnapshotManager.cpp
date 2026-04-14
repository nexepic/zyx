/**
 * @file test_SnapshotManager.cpp
 * @brief Unit tests for SnapshotManager header-only behavior.
 */

#include <gtest/gtest.h>

#include <memory>

#include "graph/storage/SnapshotManager.hpp"

using namespace graph;
using namespace graph::storage;

TEST(SnapshotManagerTest, StartsWithEmptySnapshotAndNextIdOne) {
	SnapshotManager manager;

	auto snap = manager.acquireSnapshot();
	ASSERT_NE(snap, nullptr);
	EXPECT_EQ(snap->snapshotId, 0UL);
	EXPECT_EQ(manager.getNextSnapshotId(), 1UL);
}

TEST(SnapshotManagerTest, PublishAssignsMonotonicSnapshotIds) {
	SnapshotManager manager;

	auto first = std::make_shared<CommittedSnapshot>();
	manager.publishSnapshot(first);
	EXPECT_EQ(first->snapshotId, 1UL);
	EXPECT_EQ(manager.getNextSnapshotId(), 2UL);
	EXPECT_EQ(manager.acquireSnapshot().get(), first.get());

	auto second = std::make_shared<CommittedSnapshot>();
	manager.publishSnapshot(second);
	EXPECT_EQ(second->snapshotId, 2UL);
	EXPECT_EQ(manager.getNextSnapshotId(), 3UL);
	EXPECT_EQ(manager.acquireSnapshot().get(), second.get());
}

TEST(SnapshotManagerTest, AcquireReturnsImmutableSnapshot) {
	SnapshotManager manager;

	// Publish a snapshot with some data
	auto snap = std::make_shared<CommittedSnapshot>();
	Node node(42, 1);
	snap->nodes[42] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, node);
	manager.publishSnapshot(snap);

	// Acquire it
	auto acquired = manager.acquireSnapshot();
	ASSERT_NE(acquired, nullptr);
	EXPECT_EQ(acquired->nodes.size(), 1u);
	EXPECT_TRUE(acquired->nodes.contains(42));

	// Publish a new (empty) snapshot — old acquired snapshot remains intact
	auto newSnap = std::make_shared<CommittedSnapshot>();
	manager.publishSnapshot(newSnap);

	auto latest = manager.acquireSnapshot();
	EXPECT_TRUE(latest->nodes.empty());

	// Old snapshot still has data (shared_ptr keeps it alive)
	EXPECT_EQ(acquired->nodes.size(), 1u);
}

