/**
 * @file test_NativeGraphWritePlanner_LeaseRecovery.cpp
 * @brief Durable ID-lease and publication recovery tests for native graph writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

} // namespace

class NativeGraphWritePlannerLeaseRecoveryTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest, SharesDurableLogicalIdLeaseStreamAcrossPlannerContexts) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.logical_id_lease_block_size = 4U;
	NativeGraphWritePlanner firstPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	NativeGraphWritePlanner secondPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t firstId = before.superblock().next_logical_node_id;
	before.release();

	NativeGraphWriteTransaction first = firstPlanner.beginTransaction();
	EXPECT_EQ(first.createNode({}), firstId);
	NativeGraphWriteTransaction second = secondPlanner.beginTransaction();
	EXPECT_EQ(second.createNode({}), firstId + 1U);
	first.rollback();
	second.rollback();

	NativeGraphWriteTransaction third = firstPlanner.beginTransaction();
	EXPECT_EQ(third.createNode({}), firstId + 2U);
	third.rollback();
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest, RejectsAReleasedLogicalIdRangeEvenBelowTheDurableHighWaterMark) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction cancelled = planner.beginTransaction();
	const uint64_t abandonedId = cancelled.createNode({});
	cancelled.rollback();

	NativeGraphLogicalIdReservation forged;
	forged.node_ranges = {{abandonedId, 1U}};
	NativeGraphWriteTransaction attacker = planner.beginTransaction();
	attacker.adoptLogicalIdReservation(forged);
	EXPECT_EQ(attacker.createNodeWithReservedId(abandonedId, {}), abandonedId);
	EXPECT_THROW(static_cast<void>(attacker.commit()), FormatException);
	EXPECT_EQ(attacker.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest, RejectsDuplicateReservationOnlyPublicationWithoutAdvancingGeneration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(2U, 1U);
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock initial = before.superblock();
	before.release();

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	winner.adoptLogicalIdReservation(reservation);
	NativeGraphWriteTransaction duplicate = planner.beginTransaction();
	duplicate.adoptLogicalIdReservation(reservation);

	const NativeGraphWriteCommitResult committed = winner.commit();
	EXPECT_TRUE(committed.committed());
	EXPECT_EQ(committed.superblock, initial);
	EXPECT_EQ(winner.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

	const NativeGraphWriteCommitResult rejected = duplicate.commit();
	EXPECT_EQ(rejected.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(rejected.superblock, Superblock{});
	EXPECT_EQ(duplicate.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock(), initial);
	after.release();
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest,
	   RecoversOldGenerationWhenSuperblockWriteIsInterruptedBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 4U>{7U, 9U, 11U, 13U});
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();

	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, nodeId)), FormatException);
	recovered.release();
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest, RecoversNewGenerationWhenSuperblockWriteCompletesBeforeInterruption) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<uint64_t, 4U> labels = {7U, 9U, 11U, 13U};
	const uint64_t nodeId = transaction.createNode(labels);
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();

	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	const NodeEntityRecord node = entities_->readNode(recovered, nodeId);
	EXPECT_EQ(labels_->readNodeLabels(recovered, node), std::vector<uint64_t>(labels.begin(), labels.end()));
	recovered.release();
}
