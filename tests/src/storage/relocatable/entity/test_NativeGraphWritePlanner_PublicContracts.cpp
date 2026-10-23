/**
 * @file test_NativeGraphWritePlanner_PublicContracts.cpp
 * @brief Public contract tests for native graph write transactions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
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
	options.maximum_global_adjacency_tree_pages =
			std::numeric_limits<size_t>::max() / RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U;
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_write_entries = 0U;
	rejects(options);
	options = plannerOptions();
	options.maximum_global_adjacency_write_entries = std::numeric_limits<size_t>::max();
	rejects(options);
	options = plannerOptions();
	options.global_adjacency_bulk_rebuild_entry_threshold = 0U;
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

TEST(NativeGraphWritePlannerOptionsTest, DefaultAdjacencyBudgetAccountsForBothDirections) {
	const NativeGraphWritePlannerOptions options;
	EXPECT_EQ(options.maximum_logical_id_reservation,
			  NativeGraphWritePlannerOptions::DEFAULT_MAXIMUM_LOGICAL_ID_RESERVATION);
	EXPECT_EQ(options.maximum_global_adjacency_write_entries,
			  2U * options.maximum_logical_id_reservation);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsStoresFromAnotherMetadataDomain) {
	const std::filesystem::path foreignPath = path_.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(
			foreignIo, graph::storage::test::nativeGraphInitialSuperblock());
	auto foreignEntities = std::make_unique<EntityPageStore>(foreignIo, *foreignMetadata);
	auto foreignSlottedPages = std::make_unique<SlottedPagePool>(foreignIo, *foreignMetadata);
	auto foreignIndexes = std::make_unique<BTreePageStore>(*foreignSlottedPages, *foreignMetadata);
	auto foreignLabels = std::make_unique<EntityLabelSetStore>(foreignIo, *foreignMetadata, *foreignSlottedPages);
	auto foreignProperties = std::make_unique<PropertyCollectionRootStore>(*foreignSlottedPages, *foreignMetadata);

	// The writer publishes one COW generation. Mixing typed stores from two files
	// would make its root set unrepresentable and must fail at construction.
	EXPECT_THROW(static_cast<void>(NativeGraphWritePlanner(*metadata_, *foreignEntities, *labels_, *properties_,
																								  *btree_pages_, plannerOptions())),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeGraphWritePlanner(*metadata_, *entities_, *foreignLabels, *properties_,
																								  *btree_pages_, plannerOptions())),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeGraphWritePlanner(*metadata_, *entities_, *labels_, *foreignProperties,
																								  *btree_pages_, plannerOptions())),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeGraphWritePlanner(*metadata_, *entities_, *labels_, *properties_,
																							  *foreignIndexes, plannerOptions())),
				 std::invalid_argument);

	// A transaction may only mutate B+Trees backed by the same metadata domain
	// as its graph snapshot. Validate each public scalar-index operation before
	// it can derive a plan from a foreign file.
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	EXPECT_THROW(transaction.stageScalarIndexUpsert(*foreignIndexes, 71U, {}, 1U), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexErase(*foreignIndexes, 72U, {}), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexCreate(*foreignIndexes, 73U, {}), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexDrop(*foreignIndexes, 74U, 1U), FormatException);
	transaction.rollback();

	foreignProperties.reset();
	foreignLabels.reset();
	foreignIndexes.reset();
	foreignSlottedPages.reset();
	foreignEntities.reset();
	foreignMetadata.reset();
	foreignIo.reset();
	foreignFile->close();
	foreignFile.reset();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsTransactionsFromAMovedFromPlanner) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWritePlanner destination(std::move(planner));

	EXPECT_THROW(static_cast<void>(planner.beginTransaction()), FormatException);
	EXPECT_THROW(static_cast<void>(planner.reserveLogicalIds(1U, 0U)), FormatException);
	NativeGraphWriteTransaction transaction = destination.beginTransaction();
	EXPECT_TRUE(transaction.isActive());
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
		MovesActiveTransactionsWithoutPublishingTheDiscardedTargetState) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction source = planner.beginTransaction();
	const uint64_t sourceNode = source.createNode(std::array<uint64_t, 0U>{});

	// Self move-assignment is a no-op: the active snapshot and its outstanding
	// logical-ID lease must remain attached to the same transaction.
	source.operator=(static_cast<NativeGraphWriteTransaction &&>(source));
	EXPECT_TRUE(source.isActive());

	NativeGraphWriteTransaction target = planner.beginTransaction();
	const uint64_t discardedNode = target.createNode(std::array<uint64_t, 0U>{});
	EXPECT_NE(sourceNode, discardedNode);

	// Replacing an active target must abandon its staged state and transfer the
	// source snapshot/lease exactly once. Only the source Node may publish.
	target = std::move(source);
	EXPECT_FALSE(source.isActive());
	EXPECT_TRUE(target.isActive());
	EXPECT_THROW(static_cast<void>(source.createNode(std::array<uint64_t, 0U>{})), FormatException);

	// Moving an active target again must keep the same lease/snapshot ownership;
	// the moved-from object may not retain a second path to publication.
	NativeGraphWriteTransaction finalTransaction(std::move(target));
	EXPECT_FALSE(target.isActive());
	EXPECT_TRUE(finalTransaction.isActive());
	EXPECT_THROW(static_cast<void>(target.createNode(std::array<uint64_t, 0U>{})), FormatException);
	EXPECT_NO_THROW(static_cast<void>(finalTransaction.commit()));

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_NO_THROW(static_cast<void>(entities_->readNode(snapshot, sourceNode)));
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, discardedNode)), FormatException);
	snapshot.release();
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

TEST_F(NativeGraphWritePlannerPublicContractsTest, ReservesOnlyBoundedNonemptyLogicalIdentityRanges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const uint64_t maximum = static_cast<uint64_t>(plannerOptions().maximum_logical_id_reservation);
	ASSERT_LT(maximum, std::numeric_limits<uint64_t>::max());

	EXPECT_THROW(static_cast<void>(planner.reserveLogicalIds(0U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(planner.reserveLogicalIds(maximum + 1U, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(planner.reserveLogicalIds(0U, maximum + 1U)), FormatException);

	const NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(1U, 1U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	EXPECT_EQ(reservation.node_ranges.front().count, 1U);
	EXPECT_EQ(reservation.edge_ranges.front().count, 1U);
	metadata_->abandonNativeGraphLogicalIdReservation(reservation);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, ReservesNodeAndEdgeIdentityRangesIndependently) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());

	NativeGraphLogicalIdReservation nodeOnly = planner.reserveLogicalIds(2U, 0U);
	ASSERT_FALSE(nodeOnly.node_ranges.empty());
	EXPECT_TRUE(nodeOnly.edge_ranges.empty());
	EXPECT_EQ(nodeOnly.node_ranges.front().count, 2U);

	NativeGraphLogicalIdReservation edgeOnly = planner.reserveLogicalIds(0U, 2U);
	EXPECT_TRUE(edgeOnly.node_ranges.empty());
	ASSERT_FALSE(edgeOnly.edge_ranges.empty());
	EXPECT_EQ(edgeOnly.edge_ranges.front().count, 2U);

	metadata_->abandonNativeGraphLogicalIdReservation(nodeOnly);
	metadata_->abandonNativeGraphLogicalIdReservation(edgeOnly);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
		RejectsGraphPublicationWhenARequiredLeaseExhaustsTheCheckpointLsn) {
	// Node creation obtains a durable logical-ID lease first. Starting one LSN
	// below exhaustion lets that lease succeed, then verifies that the later
	// graph publication refuses to wrap its required checkpoint LSN.
	Superblock nearExhaustion = graph::storage::test::nativeGraphInitialSuperblock();
	nearExhaustion.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 1U);
	properties_.reset();
	labels_.reset();
	btree_pages_.reset();
	slotted_pages_.reset();
	entities_.reset();
	metadata_.reset();
	io_.reset();
	file_->close();
	file_ = std::make_shared<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file_->is_open());
	io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	metadata_ = RelocatableMetadataStore::initializeNew(io_, nearExhaustion);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
	labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 0U>{});
	EXPECT_THROW(static_cast<void>(transaction.commit()), FormatException);
	// The LSN guard runs before any materialization or publication, so this is a
	// retryable planning failure rather than an uncertain-durability outcome.
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);

	MetadataSnapshotPin unchanged = metadata_->openSnapshot();
	EXPECT_EQ(unchanged.superblock().checkpoint_lsn, LogSequenceNumber(std::numeric_limits<uint64_t>::max()));
	EXPECT_EQ(unchanged.superblock().next_logical_node_id, nodeId + 1U);
	EXPECT_THROW(static_cast<void>(entities_->readNode(unchanged, nodeId)), FormatException);
	unchanged.release();

	transaction.rollback();
	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().checkpoint_lsn, LogSequenceNumber(std::numeric_limits<uint64_t>::max()));
	EXPECT_EQ(recovered.superblock().next_logical_node_id, nodeId + 1U);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, nodeId)), FormatException);
	recovered.release();
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

TEST(NativeGraphWriteTransactionPublicContractsTest, DefaultTransactionIsInactiveAndRejectsOperations) {
	// The default constructor exists for move-safe container/storage use. It must
	// remain fail-closed if a caller accidentally invokes a transaction operation
	// before attaching a planner snapshot.
	NativeGraphWriteTransaction transaction;
	EXPECT_FALSE(transaction.isActive());
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(transaction.baseSuperblock()), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.baseSnapshot()), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createNode(std::array<uint64_t, 0U>{})), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createEdge(1U, 2U, 3U)), FormatException);
	EXPECT_NO_THROW(transaction.rollback());
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
	// A nonzero identity outside the adopted range is distinct from the invalid
	// zero sentinel and must not be accepted as a leased ID.
	EXPECT_THROW(static_cast<void>(transaction.createNodeWithReservedId(nodeId + 2U, std::array<uint64_t, 0U>{})),
				 FormatException);
	EXPECT_EQ(transaction.createNodeWithReservedId(nodeId, std::array<uint64_t, 0U>{}), nodeId);
	EXPECT_THROW(static_cast<void>(transaction.createNodeWithReservedId(nodeId, std::array<uint64_t, 0U>{})),
				 FormatException);
	EXPECT_THROW(transaction.adoptLogicalIdReservation(std::move(reservation)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createEdgeWithReservedId(0U, nodeId, nodeId, 7U)), FormatException);
	EXPECT_THROW(static_cast<void>(transaction.createEdgeWithReservedId(edgeId + 1U, nodeId, nodeId, 7U)),
				 FormatException);
	EXPECT_EQ(transaction.createEdgeWithReservedId(edgeId, nodeId, nodeId, 7U), edgeId);
	EXPECT_THROW(static_cast<void>(transaction.createEdgeWithReservedId(edgeId, nodeId, nodeId, 7U)),
				 FormatException);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
		RejectsAdoptionAfterAcquiringTransactionLocalLogicalIdRanges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.reserveNodeIds(1U);

	NativeGraphLogicalIdReservation externalReservation = planner.reserveLogicalIds(1U, 0U);
	EXPECT_THROW(transaction.adoptLogicalIdReservation(externalReservation), FormatException);
	EXPECT_TRUE(transaction.isActive());

	transaction.rollback();
	metadata_->abandonNativeGraphLogicalIdReservation(externalReservation);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   PublishesReservedEdgeBetweenDistinctReservedNodes) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(2U, 1U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);

	const uint64_t source = reservation.node_ranges.front().first_id;
	const uint64_t target = source + 1U;
	const uint64_t edgeId = reservation.edge_ranges.front().first_id;
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.adoptLogicalIdReservation(std::move(reservation));
	EXPECT_EQ(transaction.createNodeWithReservedId(source, std::array<uint64_t, 0U>{}), source);
	EXPECT_EQ(transaction.createNodeWithReservedId(target, std::array<uint64_t, 0U>{}), target);
	EXPECT_EQ(transaction.createEdgeWithReservedId(edgeId, source, target, 7U), edgeId);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(snapshot, source), NodeEntityRecord{});
	EXPECT_EQ(entities_->readNode(snapshot, target), NodeEntityRecord{});
	EXPECT_EQ(entities_->readEdge(snapshot, edgeId), (EdgeEntityRecord{source, target, 7U}));
	snapshot.release();
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
	EXPECT_THROW(create.stageScalarIndexErase(*btree_pages_, indexId, conflictingKey), FormatException);
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

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsMalformedPreplannedScalarIndexCreationBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<BTreePageEntry, 1U> entries = {
			BTreePageEntry{std::vector<std::byte>{std::byte{0x3AU}}, 17U},
	};
	const auto makePlan = [this, &entries](uint64_t indexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		BTreeInitialTreeMutationPlan plan = btree_pages_->planInitialTreeMutation(snapshot, indexId, entries);
		snapshot.release();
		return plan;
	};
	const auto expectRejected = [&planner, this](BTreeInitialTreeMutationPlan plan) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		EXPECT_THROW(transaction.stageScalarIndexCreate(*btree_pages_, std::move(plan)), FormatException);
		EXPECT_TRUE(transaction.isActive());
		transaction.rollback();
	};

	BTreeInitialTreeMutationPlan zeroId = makePlan(71U);
	zeroId.index_id = 0U;
	expectRejected(std::move(zeroId));

	BTreeInitialTreeMutationPlan reservedId = makePlan(72U);
	reservedId.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
	expectRejected(std::move(reservedId));

	BTreeInitialTreeMutationPlan missingPreparer = makePlan(73U);
	missingPreparer.preparer = {};
	expectRejected(std::move(missingPreparer));

	BTreeInitialTreeMutationPlan missingPool = makePlan(74U);
	missingPool.new_pool_mutations.clear();
	expectRejected(std::move(missingPool));

	NativeGraphWriteTransaction duplicateCreation = planner.beginTransaction();
	duplicateCreation.stageScalarIndexCreate(*btree_pages_, makePlan(81U));
	EXPECT_THROW(duplicateCreation.stageScalarIndexCreate(*btree_pages_, 81U, entries), FormatException);
	EXPECT_THROW(duplicateCreation.stageScalarIndexCreate(*btree_pages_, makePlan(81U)), FormatException);
	duplicateCreation.rollback();

	NativeGraphWriteTransaction conflictingMutation = planner.beginTransaction();
	const std::array<std::byte, 1U> mutationKey = {std::byte{0x81U}};
	conflictingMutation.stageScalarIndexUpsert(*btree_pages_, 82U, mutationKey, 18U);
	EXPECT_THROW(conflictingMutation.stageScalarIndexCreate(*btree_pages_, 82U, entries), FormatException);
	EXPECT_THROW(conflictingMutation.stageScalarIndexCreate(*btree_pages_, makePlan(82U)), FormatException);
	conflictingMutation.rollback();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	for (const uint64_t indexId: {71U, 72U, 73U, 74U, 81U, 82U}) {
		EXPECT_FALSE(metadata_->findIndexRootOptional(after, indexId).has_value());
	}
	after.release();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsScalarIndexMutationsThatChangeTheirPageStoreIdentity) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	BTreePageStore alternatePages(*slotted_pages_, *metadata_);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<std::byte, 1U> key = {std::byte{0x5AU}};

	transaction.stageScalarIndexUpsert(*btree_pages_, 71U, key, 9U);
	EXPECT_THROW(transaction.stageScalarIndexUpsert(alternatePages, 71U, key, 10U), FormatException);
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

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsAStaleScalarIndexWriteInsteadOfRebasingItOntoAnUnrelatedPublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	const std::array<std::byte, 1U> key = {std::byte{0x5AU}};
	stale.stageScalarIndexUpsert(*btree_pages_, 71U, key, nodeId);

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	winner.replaceNodeProperty({nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}});
	ASSERT_TRUE(winner.commit().committed());

	const NativeGraphWriteCommitResult rejected = stale.commit();
	EXPECT_EQ(rejected.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}),
			  (PropertyCollectionRoot{nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}}));
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, 71U).has_value());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsAStaleScalarIndexCreationInsteadOfRebasingItOntoAnUnrelatedPublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	const std::array<std::byte, 1U> key = {std::byte{0x6BU}};
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
	stale.stageScalarIndexCreate(*btree_pages_, 71U, entries);

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	winner.replaceNodeProperty({nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}});
	ASSERT_TRUE(winner.commit().committed());

	const NativeGraphWriteCommitResult rejected = stale.commit();
	EXPECT_EQ(rejected.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}),
			  (PropertyCollectionRoot{nodeId, Node::typeId, {{17U, graph::PropertyValue(int64_t(22))}}}));
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, 71U).has_value());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerPublicContractsTest,
	   RejectsAnAdjacencyRebaseWhenAConcurrentCommitDeletesItsPersistedEndpoint) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = seed.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(seed.commit().committed());

	NativeGraphWriteTransaction stale = planner.beginTransaction();
	const uint64_t edgeId = stale.createEdge(source, target, 7U);

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseNode(source);
	ASSERT_TRUE(erase.commit().committed());

	// A rebase may move a disjoint adjacency overlay to a newer global root, but
	// it must never publish an Edge whose persisted endpoint was deleted after
	// the transaction's pinned snapshot.
	const NativeGraphWriteCommitResult rejected = stale.commit();
	EXPECT_EQ(rejected.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, source)), FormatException);
	EXPECT_EQ(entities_->readNode(snapshot, target), NodeEntityRecord{});
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, edgeId)), FormatException);
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

TEST_F(NativeGraphWritePlannerPublicContractsTest,
		CancelsAnUnpublishedSelfLoopWithoutLeavingEitherAdjacencyDirection) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t selfLoop = transaction.createEdge(source, source, 7U);
	const uint64_t ordinaryEdge = transaction.createEdge(source, target, 9U);

	// Removing an Edge that exists only in this transaction must discard both of
	// its provisional adjacency keys. The remaining Edge still proves the final
	// graph publication has not been reduced to a no-op.
	transaction.eraseEdge(selfLoop);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(snapshot, source), NodeEntityRecord{});
	EXPECT_EQ(entities_->readNode(snapshot, target), NodeEntityRecord{});
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, selfLoop)), FormatException);
	EXPECT_EQ(entities_->readEdge(snapshot, ordinaryEdge), (EdgeEntityRecord{source, target, 9U}));
	snapshot.release();
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

TEST_F(NativeGraphWritePlannerPublicContractsTest, RejectsPublicMutationOfFreeSpaceSummaryNamespace) {
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    auto tx = planner.beginTransaction();
    for (const auto id : {RSS_NODE_FREE_SPACE_INDEX_ID, RSS_EDGE_FREE_SPACE_INDEX_ID}) {
        EXPECT_THROW(tx.stageScalarIndexUpsert(*btree_pages_, id, {}, 1U), FormatException);
        EXPECT_THROW(tx.stageScalarIndexErase(*btree_pages_, id, {}), FormatException);
        EXPECT_THROW(tx.stageScalarIndexCreate(*btree_pages_, id, {}), FormatException);
        EXPECT_THROW(tx.stageScalarIndexDrop(*btree_pages_, id, 1U), FormatException);
    }
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, MissingFreeSpaceCandidateFailsIntegrityAndColdInsertion) {
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    auto tx = planner.beginTransaction();
    const auto node = tx.createNode(std::array<uint64_t, 0U>{});
    ASSERT_TRUE(tx.commit().committed());
    auto snapshot = metadata_->openSnapshot();
    const auto location = metadata_->findNodePageLocation(snapshot, node);
    const auto key = EntityFreeSpaceIndex::entryKey(location.page_id,
        uint16_t(EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE) - 1U), EntityPageRecordKind::EPRK_NODE);
    const auto lsn = LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
    snapshot.release();
    ASSERT_TRUE(btree_pages_->erase(RSS_NODE_FREE_SPACE_INDEX_ID, key, lsn));
    EXPECT_THROW((void)RelocatableIntegrityScanner(io_, *metadata_).scan(), FormatException);
    NativeGraphWritePlanner cold(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    auto insertion = cold.beginTransaction();
    (void)insertion.createNode(std::array<uint64_t, 0U>{});
    EXPECT_THROW((void)insertion.commit(), FormatException);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, EdgeEndpointReadsDoNotRewriteNodePagesButLabelChangesDo) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	auto seed = planner.beginTransaction();
	const auto source = seed.createNode(std::array<uint64_t, 0U>{});
	const auto target = seed.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(seed.commit().committed());
	auto old = metadata_->openSnapshot();
	const auto nodeLocation = metadata_->findNodePageLocation(old, source);
	const auto physical = metadata_->findPageLocation(old, nodeLocation.page_id);
	auto create = planner.beginTransaction();
	const auto edge = create.createEdge(source, target, 7U);
	const auto loop = create.createEdge(source, source, 8U);
	ASSERT_TRUE(create.commit().committed());
	auto current = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findNodePageLocation(current, source), nodeLocation);
	EXPECT_EQ(metadata_->findPageLocation(current, nodeLocation.page_id), physical);
	EXPECT_EQ(entities_->readEdge(current, edge).target_node_id, target);
	EXPECT_EQ(entities_->readEdge(current, loop).target_node_id, source);
	auto inspect = planner.beginTransaction();
	EXPECT_EQ(inspect.collectIncidentEdgeIds(source, 4U).size(), 2U);
	inspect.rollback();
	current.release();
	auto erase = planner.beginTransaction();
	erase.eraseEdge(edge); erase.eraseEdge(loop);
	ASSERT_TRUE(erase.commit().committed());
	current = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findPageLocation(current, nodeLocation.page_id), physical);
	current.release();
	auto relabel = planner.beginTransaction();
	static_cast<void>(relabel.createEdge(source, target, 9U));
	relabel.replaceNodeLabels(source, std::array<uint64_t, 1U>{42U});
	ASSERT_TRUE(relabel.commit().committed());
	current = metadata_->openSnapshot();
	EXPECT_NE(metadata_->findPageLocation(current, nodeLocation.page_id), physical);
	EXPECT_EQ(entities_->readNode(old, source), NodeEntityRecord{});
	EXPECT_EQ(entities_->readNode(current, source).inline_label_ids.front(), 42U);
	old.release(); current.release();
	reopenNativeStores();
	auto reopened = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(reopened, source).inline_label_ids.front(), 42U);
}

TEST_F(NativeGraphWritePlannerPublicContractsTest, PropertyOnlyCommitDoesNotDiscoverOrQueryUnrelatedEntityPools) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	auto seed = planner.beginTransaction();
	const auto node = seed.createNode(std::array<uint64_t, 0U>{});
	seed.replaceNodeProperty({node, Node::typeId, {{1U, graph::PropertyValue(int64_t{1})}}});
	ASSERT_TRUE(seed.commit().committed());
	auto old = metadata_->openSnapshot();
	const auto nodePage = metadata_->findNodePageLocation(old, node).page_id;
	const auto physical = metadata_->findPageLocation(old, nodePage);
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, node};
	const auto propertyLocation = metadata_->findEntityPropertyLocation(old, propertyKey);
	const auto propertyPhysical = metadata_->findPageLocation(old, propertyLocation.page_id);
	auto update = planner.beginTransaction();
	update.replaceNodeProperty({node, Node::typeId, {{1U, graph::PropertyValue(int64_t{2})}}});
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); } } restore{enabled};
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	ASSERT_TRUE(update.commit().committed());
	const auto values = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(values.contains("native_graph.commit.entity_pool_discoveries"));
	EXPECT_FALSE(values.contains("native_graph.commit.entity_pool_capacity_queries"));
	auto current = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findPageLocation(current, nodePage), physical);
	EXPECT_EQ(metadata_->findEntityPropertyLocation(current, propertyKey), propertyLocation);
	EXPECT_EQ(current.superblock().entity_property_directory_root, old.superblock().entity_property_directory_root);
	EXPECT_NE(metadata_->findPageLocation(current, propertyLocation.page_id), propertyPhysical);
	ASSERT_TRUE(values.contains("native_graph.commit.unchanged_property_locations"));
	EXPECT_EQ(values.at("native_graph.commit.unchanged_property_locations").totalValue, 1);
	EXPECT_EQ(properties_->readEntity(old, {EntityPropertyOwnerKind::EPOK_NODE, node}).entries.front().value,
			graph::PropertyValue(int64_t{1}));
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, node}).entries.front().value,
			graph::PropertyValue(int64_t{2}));
}
