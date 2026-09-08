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

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest,
	   RejectsCumulativeLogicalIdReservationBeforeAcquiringOrRetainingAnotherLease) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.logical_id_lease_block_size = 1U;
	options.maximum_logical_id_reservation = 1U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t firstNodeId = before.superblock().next_logical_node_id;
	before.release();

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.reserveNodeIds(1U);
	MetadataSnapshotPin afterFirstReservation = metadata_->openSnapshot();
	const Superblock afterFirstLease = afterFirstReservation.superblock();
	ASSERT_EQ(afterFirstLease.next_logical_node_id, firstNodeId + 1U);
	afterFirstReservation.release();

	EXPECT_THROW(transaction.reserveNodeIds(1U), FormatException);
	EXPECT_TRUE(transaction.isActive());
	MetadataSnapshotPin afterRejectedReservation = metadata_->openSnapshot();
	EXPECT_EQ(afterRejectedReservation.superblock(), afterFirstLease);
	afterRejectedReservation.release();

	// The next range was never acquired. It must therefore not be claimable as a
	// lingering process-local publication capability.
	NativeGraphLogicalIdReservation rejectedRange;
	rejectedRange.node_ranges = {{firstNodeId + 1U, 1U}};
	EXPECT_FALSE(metadata_->claimNativeGraphLogicalIdReservation(rejectedRange));
	transaction.rollback();

	// Node and Edge leases have independent limits. The Edge path must reject
	// before acquiring a second capability just as the Node path does.
	MetadataSnapshotPin beforeEdgeReservation = metadata_->openSnapshot();
	const uint64_t firstEdgeId = beforeEdgeReservation.superblock().next_logical_edge_id;
	beforeEdgeReservation.release();
	NativeGraphWriteTransaction edgeTransaction = planner.beginTransaction();
	edgeTransaction.reserveEdgeIds(1U);
	MetadataSnapshotPin afterFirstEdgeReservation = metadata_->openSnapshot();
	const Superblock afterFirstEdgeLease = afterFirstEdgeReservation.superblock();
	ASSERT_EQ(afterFirstEdgeLease.next_logical_edge_id, firstEdgeId + 1U);
	afterFirstEdgeReservation.release();

	EXPECT_THROW(edgeTransaction.reserveEdgeIds(1U), FormatException);
	EXPECT_TRUE(edgeTransaction.isActive());
	MetadataSnapshotPin afterRejectedEdgeReservation = metadata_->openSnapshot();
	EXPECT_EQ(afterRejectedEdgeReservation.superblock(), afterFirstEdgeLease);
	afterRejectedEdgeReservation.release();
	NativeGraphLogicalIdReservation rejectedEdgeRange;
	rejectedEdgeRange.edge_ranges = {{firstEdgeId + 1U, 1U}};
	EXPECT_FALSE(metadata_->claimNativeGraphLogicalIdReservation(rejectedEdgeRange));
	edgeTransaction.rollback();
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest, RejectsMalformedMixedReservationWithoutPartiallyAdoptingIt) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(1U, 1U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);

	NativeGraphLogicalIdReservation malformed = reservation;
	malformed.edge_ranges.front().count = 0U;
	NativeGraphWriteTransaction rejected = planner.beginTransaction();
	EXPECT_THROW(rejected.adoptLogicalIdReservation(malformed), FormatException);
	EXPECT_THROW(static_cast<void>(rejected.createNodeWithReservedId(reservation.node_ranges.front().first_id, {})),
				 FormatException);
	rejected.rollback();

	NativeGraphWriteTransaction accepted = planner.beginTransaction();
	accepted.adoptLogicalIdReservation(reservation);
	EXPECT_EQ(accepted.createNodeWithReservedId(reservation.node_ranges.front().first_id, {}),
			  reservation.node_ranges.front().first_id);
	accepted.rollback();
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

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest,
	   RebasesGraphWorkAcrossAnInterleavedNodeLeaseOnlyPublication) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	// A one-ID backing lease makes the intervening reservation publish a new
	// Superblock rather than consuming an already durable private remainder.
	options.logical_id_lease_block_size = 1U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	const uint64_t nodeId = stale.createNode({});
	const Superblock beforeLease = stale.baseSuperblock();

	NativeGraphLogicalIdReservation intervening = planner.reserveLogicalIds(1U, 0U);
	MetadataSnapshotPin afterLease = metadata_->openSnapshot();
	ASSERT_GT(afterLease.superblock().generation, beforeLease.generation);
	ASSERT_GT(afterLease.superblock().next_logical_node_id, beforeLease.next_logical_node_id);
	ASSERT_EQ(afterLease.superblock().next_logical_edge_id, beforeLease.next_logical_edge_id);
	afterLease.release();

	// A lease publication changes no graph-visible root. The stale write may
	// therefore retarget the newer metadata snapshot and publish its complete
	// graph generation, instead of reporting a false write conflict.
	const NativeGraphWriteCommitResult committed = stale.commit();
	EXPECT_TRUE(committed.committed());
	EXPECT_GT(committed.superblock.generation, beforeLease.generation);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(current, nodeId), NodeEntityRecord{});
	current.release();
	metadata_->abandonNativeGraphLogicalIdReservation(intervening);
}

TEST_F(NativeGraphWritePlannerLeaseRecoveryTest,
	   RebasesGraphWorkAcrossAnInterleavedEdgeLeaseOnlyPublication) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.logical_id_lease_block_size = 1U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	const uint64_t nodeId = stale.createNode({});
	const Superblock beforeLease = stale.baseSuperblock();

	NativeGraphLogicalIdReservation intervening = planner.reserveLogicalIds(0U, 1U);
	MetadataSnapshotPin afterLease = metadata_->openSnapshot();
	ASSERT_GT(afterLease.superblock().generation, beforeLease.generation);
	ASSERT_EQ(afterLease.superblock().next_logical_node_id, beforeLease.next_logical_node_id + 1U);
	ASSERT_GT(afterLease.superblock().next_logical_edge_id, beforeLease.next_logical_edge_id);
	afterLease.release();

	const NativeGraphWriteCommitResult committed = stale.commit();
	EXPECT_TRUE(committed.committed());
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(current, nodeId), NodeEntityRecord{});
	current.release();
	metadata_->abandonNativeGraphLogicalIdReservation(intervening);
}
