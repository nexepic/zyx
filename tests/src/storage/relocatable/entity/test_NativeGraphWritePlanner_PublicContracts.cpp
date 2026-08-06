/**
 * @file test_NativeGraphWritePlanner_PublicContracts.cpp
 * @brief Public contract tests for native graph write transactions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	PropertyCollectionRoot nodePropertyRoot(uint64_t nodeId) { return {nodeId, Node::typeId, {}}; }

} // namespace

class NativeGraphWritePlannerPublicContractsTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsEveryUnboundedOrInvalidPlannerOption) {
	const auto rejects = [this](const NativeGraphWritePlannerOptions &options) {
		EXPECT_THROW(static_cast<void>(NativeGraphWritePlanner(*metadata_, *entities_, *labels_, *properties_,
															   *btree_pages_, options)),
					 std::invalid_argument);
	};

	NativeGraphWritePlannerOptions options = plannerOptions();
	options.first_slot = PhysicalSlotId{};
	rejects(options);
	options = plannerOptions();
	options.last_slot = PhysicalSlotId{};
	rejects(options);
	options = plannerOptions();
	options.first_slot = PhysicalSlotId(81U);
	rejects(options);
	options = plannerOptions();
	options.entity_pool_owner_id = 0U;
	rejects(options);

	options = plannerOptions();
	options.maximum_adjacency_read_edges = 0U;
	rejects(options);
	options = plannerOptions();
	options.maximum_adjacency_read_edges = std::numeric_limits<size_t>::max();
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_tree_pages = 0U;
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_tree_pages = std::numeric_limits<size_t>::max();
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_write_entries = 0U;
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_write_entries = std::numeric_limits<size_t>::max();
	rejects(options);
	options = plannerOptions();
	options.logical_id_lease_block_size = 0U;
	rejects(options);
	options = plannerOptions();
	options.logical_id_lease_block_size = std::numeric_limits<size_t>::max();
	rejects(options);
	options = plannerOptions();
	options.maximum_logical_id_reservation = 0U;
	rejects(options);
	options = plannerOptions();
	options.maximum_logical_id_reservation = std::numeric_limits<size_t>::max();
	rejects(options);

	options = plannerOptions();
	options.property_roots.first_slot = PhysicalSlotId{};
	rejects(options);
	options = plannerOptions();
	options.property_roots.last_slot = PhysicalSlotId{};
	rejects(options);
	options = plannerOptions();
	options.property_roots.first_slot = PhysicalSlotId(81U);
	rejects(options);
	options = plannerOptions();
	options.property_roots.page_owner_id = 0U;
	rejects(options);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsOperationsOnDefaultAndCompletedTransactions) {
	NativeGraphWriteTransaction detached;
	EXPECT_FALSE(detached.isActive());
	EXPECT_EQ(detached.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(detached.baseSuperblock()), FormatException);
	EXPECT_THROW(static_cast<void>(detached.baseSnapshot()), FormatException);
	EXPECT_THROW(static_cast<void>(detached.createNode(std::array<uint64_t, 0U>{})), FormatException);
	EXPECT_THROW(static_cast<void>(detached.createEdge(1U, 1U, 7U)), FormatException);
	EXPECT_THROW(detached.reserveNodeIds(1U), FormatException);
	EXPECT_THROW(static_cast<void>(detached.collectIncidentEdgeIds(1U, 1U)), FormatException);
	EXPECT_THROW(detached.replaceNodeLabels(1U, std::array<uint64_t, 0U>{}), FormatException);
	EXPECT_THROW(detached.replaceNodeProperty(nodePropertyRoot(1U)), FormatException);
	EXPECT_THROW(detached.eraseEdge(1U), FormatException);
	EXPECT_THROW(static_cast<void>(detached.commit()), FormatException);

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	ASSERT_TRUE(transaction.isActive());
	ASSERT_TRUE(transaction.baseSnapshot().isValid());
	EXPECT_EQ(transaction.baseSuperblock(), transaction.baseSnapshot().superblock());
	transaction.rollback();
	EXPECT_FALSE(transaction.isActive());
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(transaction.createNode(std::array<uint64_t, 0U>{})), FormatException);
	EXPECT_THROW(transaction.replaceEdgeProperty({1U, Edge::typeId, {}}), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexUpsert(*btree_pages_, 7U, {}, 1U), FormatException);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsInvalidLabelsBeforeLeasingLogicalNodeIds) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t initialHighWater = metadata_->openSnapshot().superblock().next_logical_node_id;

	const std::array<uint64_t, 1U> zero = {0U};
	const std::array<uint64_t, 2U> unordered = {9U, 7U};
	const std::array<uint64_t, 2U> duplicate = {7U, 7U};
	std::vector<uint64_t> tooMany(RSS_ENTITY_MAX_LABELS + 1U, 1U);
	for (size_t index = 0U; index < tooMany.size(); ++index) {
		tooMany[index] = static_cast<uint64_t>(index + 1U);
	}

	EXPECT_THROW(static_cast<void>(transaction.createNode(zero)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createNode(unordered)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createNode(duplicate)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createNode(tooMany)), FormatException);
	EXPECT_EQ(metadata_->openSnapshot().superblock().next_logical_node_id, initialHighWater);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsInvalidActiveTransactionInputsBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction empty = planner.beginTransaction();
	EXPECT_THROW(static_cast<void>(empty.commit()), FormatException);
	EXPECT_TRUE(empty.isActive());
	empty.rollback();

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t initialNodeHighWater = metadata_->openSnapshot().superblock().next_logical_node_id;
	const uint64_t initialEdgeHighWater = metadata_->openSnapshot().superblock().next_logical_edge_id;

	transaction.reserveNodeIds(0U);
	transaction.reserveEdgeIds(0U);
	EXPECT_THROW(static_cast<void>(transaction.collectIncidentEdgeIds(0U, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.collectIncidentEdgeIds(1U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.nodeHasIncidentEdges(0U)), FormatException);
	EXPECT_THROW(transaction.replaceNodeProperty({0U, Node::typeId, {}}), FormatException);
	EXPECT_THROW(transaction.replaceNodeProperty({1U, Edge::typeId, {}}), FormatException);
	EXPECT_THROW(transaction.replaceEdgeProperty({0U, Edge::typeId, {}}), FormatException);
	EXPECT_THROW(transaction.replaceEdgeProperty({1U, Node::typeId, {}}), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexUpsert(*btree_pages_, 0U, {}, 1U), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexErase(*btree_pages_, RSS_GLOBAL_ADJACENCY_INDEX_ID, {}), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexDrop(*btree_pages_, RSS_LARGE_VALUE_CONTENT_INDEX_ID, 1U),
				 FormatException);
	EXPECT_THROW(transaction.stageScalarIndexDrop(*btree_pages_, 7U, 0U), FormatException);
	EXPECT_EQ(metadata_->openSnapshot().superblock().next_logical_node_id, initialNodeHighWater);
	EXPECT_EQ(metadata_->openSnapshot().superblock().next_logical_edge_id, initialEdgeHighWater);

	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	static_cast<void>(transaction.createEdge(source, target, 7U));
	EXPECT_THROW(transaction.eraseNode(source), FormatException);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsZeroEntityIdentitiesAndAdjacencyReadsForAnAlreadyErasedNodeBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	MetadataSnapshotPin beforeInvalidIdentity = metadata_->openSnapshot();
	const uint64_t initialNodeHighWater = beforeInvalidIdentity.superblock().next_logical_node_id;
	const uint64_t initialEdgeHighWater = beforeInvalidIdentity.superblock().next_logical_edge_id;
	beforeInvalidIdentity.release();

	EXPECT_THROW(static_cast<void>(transaction.createEdge(0U, 1U, 7U)), FormatException);
	EXPECT_THROW(transaction.eraseEdge(0U), FormatException);
	EXPECT_THROW(transaction.replaceNodeLabels(0U, std::array<uint64_t, 0U>{}), FormatException);
	MetadataSnapshotPin afterInvalidIdentity = metadata_->openSnapshot();
	EXPECT_EQ(afterInvalidIdentity.superblock().next_logical_node_id, initialNodeHighWater);
	EXPECT_EQ(afterInvalidIdentity.superblock().next_logical_edge_id, initialEdgeHighWater);
	afterInvalidIdentity.release();
	transaction.rollback();

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseNode(nodeId);
	EXPECT_THROW(static_cast<void>(erase.collectIncidentEdgeIds(nodeId, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(erase.nodeHasIncidentEdges(nodeId)), FormatException);
	erase.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsPersistedNodeDeletionUntilEveryIncidentEdgeIsStagedForErasure) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = seed.createEdge(source, target, 7U);
	ASSERT_TRUE(seed.commit().committed());

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	EXPECT_THROW(erase.eraseNode(source), FormatException);
	EXPECT_TRUE(erase.isActive());

	// A rejected request must not leave a provisional node deletion behind. Once
	// the incident edge is staged for removal, the same transaction can proceed.
	erase.eraseEdge(edgeId);
	erase.eraseNode(source);
	ASSERT_TRUE(erase.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(entities_->readNode(current, source)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(current, edgeId)), FormatException);
	EXPECT_EQ(entities_->readNode(current, target), NodeEntityRecord{});
	current.release();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, EnforcesExactAdoptedLogicalIdReservations) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	EXPECT_THROW(static_cast<void>(planner.reserveLogicalIds(0U, 0U)), FormatException);

	NativeGraphWritePlannerOptions boundedOptions = plannerOptions();
	boundedOptions.maximum_logical_id_reservation = 2U;
	NativeGraphWritePlanner bounded(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, boundedOptions);
	EXPECT_THROW(static_cast<void>(bounded.reserveLogicalIds(3U, 0U)), FormatException);

	NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(2U, 1U);
	ASSERT_FALSE(reservation.node_ranges.empty());
	ASSERT_FALSE(reservation.edge_ranges.empty());
	const uint64_t nodeId = reservation.node_ranges.front().first_id;
	const uint64_t edgeId = reservation.edge_ranges.front().first_id;
	ASSERT_NE(nodeId, 0U);
	ASSERT_NE(edgeId, 0U);

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.adoptLogicalIdReservation(reservation);
	EXPECT_THROW(static_cast<void>(transaction.createNodeWithReservedId(nodeId - 1U, std::array<uint64_t, 0U>{})),
				 FormatException);
	EXPECT_EQ(transaction.createNodeWithReservedId(nodeId, std::array<uint64_t, 0U>{}), nodeId);
	EXPECT_THROW(static_cast<void>(transaction.createNodeWithReservedId(nodeId, std::array<uint64_t, 0U>{})),
				 FormatException);
	EXPECT_THROW(transaction.adoptLogicalIdReservation(std::move(reservation)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createEdgeWithReservedId(0U, nodeId, nodeId, 7U)), FormatException);
	EXPECT_EQ(transaction.createEdgeWithReservedId(edgeId, nodeId, nodeId, 7U), edgeId);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsMalformedAdoptedLogicalIdReservationsBeforeStagingGraphState) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.maximum_logical_id_reservation = 2U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();

	NativeGraphLogicalIdReservation zeroFirstId;
	zeroFirstId.node_ranges.push_back({0U, 1U});
	EXPECT_THROW(transaction.adoptLogicalIdReservation(std::move(zeroFirstId)), FormatException);

	NativeGraphLogicalIdReservation zeroCount;
	zeroCount.edge_ranges.push_back({1U, 0U});
	EXPECT_THROW(transaction.adoptLogicalIdReservation(std::move(zeroCount)), FormatException);

	NativeGraphLogicalIdReservation overBound;
	overBound.node_ranges.push_back({1U, 3U});
	EXPECT_THROW(transaction.adoptLogicalIdReservation(std::move(overBound)), FormatException);

	EXPECT_TRUE(transaction.isActive());
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RetiresAPublishedScalarIndexThroughTheGraphTransaction) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const uint64_t indexId = 71U;
	const std::array<BTreePageEntry, 2U> entries = {
			BTreePageEntry{{std::byte{0x01}}, 11U},
			BTreePageEntry{{std::byte{0x02}}, 22U},
	};
	const std::array<std::byte, 1U> conflictingKey = {std::byte{0x03}};

	NativeGraphWriteTransaction create = planner.beginTransaction();
	create.stageScalarIndexCreate(*btree_pages_, indexId, entries);
	EXPECT_THROW(create.stageScalarIndexUpsert(*btree_pages_, indexId, conflictingKey, 33U), FormatException);
	EXPECT_THROW(create.stageScalarIndexDrop(*btree_pages_, indexId, 16U), FormatException);
	ASSERT_TRUE(create.commit().committed());
	MetadataSnapshotPin beforeDrop = metadata_->openSnapshot();
	ASSERT_TRUE(metadata_->findIndexRootOptional(beforeDrop, indexId).has_value());
	beforeDrop.release();

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	drop.stageScalarIndexDrop(*btree_pages_, indexId, 16U);
	EXPECT_THROW(drop.stageScalarIndexUpsert(*btree_pages_, indexId, conflictingKey, 33U), FormatException);
	EXPECT_THROW(drop.stageScalarIndexErase(*btree_pages_, indexId, conflictingKey), FormatException);
	EXPECT_THROW(drop.stageScalarIndexDrop(*btree_pages_, indexId, 16U), FormatException);
	ASSERT_TRUE(drop.commit().committed());
	MetadataSnapshotPin afterDrop = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(afterDrop, indexId).has_value());
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsScalarIndexMutationsThatChangeTheirPageStoreIdentity) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	BTreePageStore alternatePages(*slotted_pages_, *metadata_);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<std::byte, 1U> key = {std::byte{0x5AU}};

	transaction.stageScalarIndexUpsert(*btree_pages_, 71U, key, 9U);
	EXPECT_THROW(transaction.stageScalarIndexErase(alternatePages, 71U, key), FormatException);
	EXPECT_TRUE(transaction.isActive());
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsAStalePropertyWriteInsteadOfRebasingItOntoAnUnrelatedPublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	stale.replaceNodeProperty({nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(11))}}});

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	winner.replaceNodeProperty({nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}});
	ASSERT_TRUE(winner.commit().committed());

	const NativeGraphWriteCommitResult rejected = stale.commit();
	EXPECT_EQ(rejected.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

	reopenNativeStores();
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}),
			  (PropertyCollectionRoot{nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}}));
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsFurtherNodeMutationAfterStagedDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseNode(nodeId);
	EXPECT_THROW(erase.replaceNodeLabels(nodeId, std::array<uint64_t, 0U>{}), FormatException);
	EXPECT_THROW(erase.replaceNodeProperty(nodePropertyRoot(nodeId)), FormatException);
	EXPECT_THROW(static_cast<void>(erase.createEdge(nodeId, nodeId, 7U)), FormatException);
	ASSERT_TRUE(erase.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, nodeId)), FormatException);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsFurtherEdgeMutationAfterStagedDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = create.createEdge(source, target, 7U);
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseEdge(edgeId);
	EXPECT_THROW(erase.eraseEdge(edgeId), FormatException);
	EXPECT_THROW(erase.replaceEdgeProperty({edgeId, Edge::typeId, {}}), FormatException);
	ASSERT_TRUE(erase.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, edgeId)), FormatException);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, MoveTransfersThePinnedTransactionAndRollsBackTheMovedFromObject) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction source = planner.beginTransaction();
	const uint64_t nodeId = source.createNode(std::array<uint64_t, 0U>{});

	NativeGraphWriteTransaction target(std::move(source));
	EXPECT_FALSE(source.isActive());
	EXPECT_EQ(source.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(source.baseSnapshot()), FormatException);
	ASSERT_TRUE(target.isActive());
	EXPECT_TRUE(target.commit().committed());
	EXPECT_FALSE(target.isActive());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(snapshot, nodeId), NodeEntityRecord{});
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, SelfMoveAssignmentKeepsThePinnedTransactionAndItsLease) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 0U>{});

	NativeGraphWriteTransaction &self = transaction;
	transaction = std::move(self);
	ASSERT_TRUE(transaction.isActive());
	EXPECT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(snapshot, nodeId), NodeEntityRecord{});
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, MoveAssignmentAbandonsTheReplacedTransactionLease) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction replaced = planner.beginTransaction();
	const uint64_t abandonedNodeId = replaced.createNode(std::array<uint64_t, 0U>{});

	NativeGraphWriteTransaction source = planner.beginTransaction();
	const uint64_t committedNodeId = source.createNode(std::array<uint64_t, 0U>{});
	replaced = std::move(source);
	EXPECT_FALSE(source.isActive());
	EXPECT_EQ(source.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);

	NativeGraphLogicalIdReservation abandoned;
	abandoned.node_ranges.push_back({abandonedNodeId, 1U});
	EXPECT_FALSE(metadata_->claimNativeGraphLogicalIdReservation(abandoned));
	ASSERT_TRUE(replaced.isActive());
	EXPECT_TRUE(replaced.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, abandonedNodeId)), FormatException);
	EXPECT_EQ(entities_->readNode(snapshot, committedNodeId), NodeEntityRecord{});
}
