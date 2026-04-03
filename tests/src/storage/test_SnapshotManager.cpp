/**
 * @file test_SnapshotManager.cpp
 * @brief Unit tests for SnapshotManager header-only behavior.
 */

#include <gtest/gtest.h>

#include <memory>

#include "graph/storage/SnapshotManager.hpp"

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

