/**
 * @file test_NativeGraphWritePlanner_Adjacency.cpp
 * @brief Global Packed B+Tree adjacency publication tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/format/SuperblockPair.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"
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

	[[nodiscard]] std::vector<uint64_t> scan(GlobalAdjacencyIndex &index, const MetadataSnapshotPin &snapshot,
											 GlobalAdjacencyDirection direction, uint64_t nodeId) {
		const GlobalAdjacencyRange range = index.scan(snapshot, direction, nodeId, 32U);
		if (range.continuation.has_value()) {
			throw std::runtime_error("test global adjacency range unexpectedly exceeded its bound");
		}
		return range.edge_ids;
	}

	[[nodiscard]] std::vector<uint64_t> scanAll(GlobalAdjacencyIndex &index, const MetadataSnapshotPin &snapshot,
												GlobalAdjacencyDirection direction, uint64_t nodeId,
												size_t expectedMaximumResults) {
		std::vector<uint64_t> result;
		std::optional<GlobalAdjacencyContinuation> continuation;
		do {
			const GlobalAdjacencyRange page = index.scan(snapshot, direction, nodeId, 13U, continuation);
			result.insert(result.end(), page.edge_ids.begin(), page.edge_ids.end());
			if (result.size() > expectedMaximumResults) {
				throw std::runtime_error("test global adjacency cursor returned more Edge IDs than its oracle");
			}
			continuation = page.continuation;
		} while (continuation.has_value());
		return result;
	}

	[[nodiscard]] size_t edgesRequiredToSplitOneAdjacencyLeaf() {
		PageOwnerHeader owner;
		owner.page_id = PageId(1U);
		owner.owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = Generation(1U);
		owner.committed_lsn = LogSequenceNumber(1U);
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		header.kind = BTreeNodeKind::BPN_LEAF;
		header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
		BTreePage page = BTreePage::create(owner, header);
		for (uint64_t edgeId = 1U;; ++edgeId) {
			try {
				page.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 1U, edgeId}), 0U});
			} catch (const FormatException &error) {
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				return static_cast<size_t>(edgeId);
			}
		}
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const LogSequenceNumber result(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		return result;
	}

	[[nodiscard]] SlottedPagePoolAllocationRequest indexPoolRequest(SegmentId segmentId, PageId pageId,
																	Generation generation, uint64_t indexId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = indexId;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = indexId;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		return result;
	}

	[[nodiscard]] BTreePage emptyIndexLeaf(const SlottedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = lsn;
		BTreePageHeader header;
		header.index_id = request.page_owner_id;
		header.page_id = request.page_id;
		header.generation = request.generation;
		return BTreePage::create(owner, header);
	}

} // namespace

class NativeGraphWritePlannerAdjacencyTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerAdjacencyTest, PublishesAuthoritativeEdgesAndBothKeyOnlyDirections) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = transaction.createEdge(source, target, 17U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::optional<IndexRootLocation> root =
			metadata_->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	ASSERT_TRUE(root.has_value());
	const BTreePage rootPage = btree_pages_->read(snapshot, root->root_page_id);
	EXPECT_EQ(rootPage.header().index_id, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	EXPECT_EQ(rootPage.header().leaf_value_layout, BTreeLeafValueLayout::BLVL_EMPTY);

	// Endpoint/type data is read exclusively from the authoritative Edge record.
	EXPECT_EQ(entities_->readEdge(snapshot, edgeId), (EdgeEntityRecord{source, target, 17U}));

	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source), (std::vector<uint64_t>{edgeId}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, target), (std::vector<uint64_t>{edgeId}));
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, source).empty());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, KeepsParallelEdgesAndSelfLoopsDistinctAcrossDirections) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstParallel = transaction.createEdge(source, target, 7U);
	const uint64_t secondParallel = transaction.createEdge(source, target, 9U);
	const uint64_t selfLoop = transaction.createEdge(source, source, 11U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{firstParallel, secondParallel, selfLoop}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, target),
			  (std::vector<uint64_t>{firstParallel, secondParallel}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, source), (std::vector<uint64_t>{selfLoop}));
	snapshot.release();

	NativeGraphWriteTransaction detach = planner.beginTransaction();
	EXPECT_EQ(detach.collectIncidentEdgeIds(source, 4U),
			  (std::vector<uint64_t>{firstParallel, secondParallel, selfLoop}));
	detach.rollback();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, BoundsStagedAdjacencyOperationsBeforeChangingEdgeState) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.maximum_global_adjacency_write_entries = 2U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = transaction.createEdge(source, firstTarget, 7U);

	EXPECT_THROW(static_cast<void>(transaction.createEdge(source, secondTarget, 9U)), FormatException);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source), (std::vector<uint64_t>{firstEdge}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, firstTarget),
			  (std::vector<uint64_t>{firstEdge}));
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, secondTarget).empty());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, CancelsProvisionalAdjacencyKeysWhenAnEdgeIsErasedBeforeCommit) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = transaction.createEdge(source, target, 7U);
	transaction.replaceEdgeProperty({edgeId, graph::Edge::typeId, {{17U, graph::PropertyValue(int64_t(42))}}});

	// The transaction owns both key-only directions. Deleting the provisional
	// Edge must cancel them rather than materializing an otherwise empty global
	// tree or retaining an unreachable Edge record.
	transaction.eraseEdge(edgeId);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, edgeId)), FormatException);
	EXPECT_FALSE(metadata_
						 ->findEntityPropertyLocationOptional(
								 snapshot, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, edgeId})
						 .has_value());
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source).empty());
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, target).empty());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, DiscardsAnUnreferencedProvisionalNodeBeforeCommit) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{7U});
	transaction.eraseNode(nodeId);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(snapshot, nodeId).has_value());
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, nodeId)), FormatException);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, PreservesPinnedOldTreeAndErasesBothDirectionalKeys) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = create.createEdge(source, target, 7U);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, before, GlobalAdjacencyDirection::GAD_OUT, source), (std::vector<uint64_t>{edgeId}));

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseEdge(edgeId);
	ASSERT_TRUE(erase.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_TRUE(scan(adjacency, after, GlobalAdjacencyDirection::GAD_OUT, source).empty());
	EXPECT_TRUE(scan(adjacency, after, GlobalAdjacencyDirection::GAD_IN, target).empty());
	EXPECT_THROW(static_cast<void>(entities_->readEdge(after, edgeId)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(after, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	// The old root remains available until the pinned snapshot drains.
	EXPECT_EQ(scan(adjacency, before, GlobalAdjacencyDirection::GAD_OUT, source), (std::vector<uint64_t>{edgeId}));
	after.release();
	before.release();

	NativeGraphWriteTransaction recreate = planner.beginTransaction();
	const uint64_t replacement = recreate.createEdge(source, target, 9U);
	ASSERT_TRUE(recreate.commit().committed());
	MetadataSnapshotPin recreated = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(recreated, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_EQ(scan(adjacency, recreated, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{replacement}));
	recreated.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, RebasesDisjointDeleteOnlyOverlaysOnTheCurrentGlobalRoot) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = seed.createEdge(source, firstTarget, 7U);
	const uint64_t secondEdge = seed.createEdge(source, secondTarget, 9U);
	ASSERT_TRUE(seed.commit().committed());

	NativeGraphWriteTransaction first = planner.beginTransaction();
	NativeGraphWriteTransaction second = planner.beginTransaction();
	first.eraseEdge(firstEdge);
	second.eraseEdge(secondEdge);
	ASSERT_TRUE(first.commit().committed());
	EXPECT_TRUE(second.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source).empty());
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, firstTarget).empty());
	EXPECT_TRUE(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, secondTarget).empty());
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, firstEdge)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, secondEdge)), FormatException);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, RebasesConcurrentLeasedEdgeCreationsWithoutLosingEitherDirection) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = seed.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(seed.commit().committed());

	NativeGraphWriteTransaction first = planner.beginTransaction();
	NativeGraphWriteTransaction second = planner.beginTransaction();
	const uint64_t firstEdge = first.createEdge(source, firstTarget, 7U);
	const uint64_t secondEdge = second.createEdge(source, secondTarget, 9U);
	EXPECT_NE(firstEdge, secondEdge);
	ASSERT_TRUE(first.commit().committed());
	EXPECT_TRUE(second.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{firstEdge, secondEdge}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, firstTarget),
			  (std::vector<uint64_t>{firstEdge}));
	EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, secondTarget),
			  (std::vector<uint64_t>{secondEdge}));
	EXPECT_EQ(entities_->readEdge(snapshot, firstEdge), (EdgeEntityRecord{source, firstTarget, 7U}));
	EXPECT_EQ(entities_->readEdge(snapshot, secondEdge), (EdgeEntityRecord{source, secondTarget, 9U}));
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, RejectsARebasedDeleteWhenTheSameEdgeChanged) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = seed.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = seed.createEdge(source, target, 7U);
	ASSERT_TRUE(seed.commit().committed());

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	NativeGraphWriteTransaction stale = planner.beginTransaction();
	winner.eraseEdge(edgeId);
	stale.eraseEdge(edgeId);
	ASSERT_TRUE(winner.commit().committed());
	EXPECT_EQ(stale.commit().outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, RecoversBothAdjacencyDirectionsAcrossEverySuperblockInterrupt) {
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWritePlanner bootstrap(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction createNodes = bootstrap.beginTransaction();
	const uint64_t source = createNodes.createNode(noLabels);
	const uint64_t target = createNodes.createNode(noLabels);
	ASSERT_TRUE(createNodes.commit().committed());

	const auto assertRecovered = [this, source, target](BootstrapPublishFailpoint failpoint, bool shouldPublish) {
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
										plannerOptions());
		NativeGraphWriteTransaction interrupted = planner.beginTransaction();
		const uint64_t edgeId = interrupted.createEdge(source, target, 51U);
		EXPECT_THROW(static_cast<void>(interrupted.commit(withBootstrapFailpoint(failpoint))),
					 BootstrapPublishInterrupted);
		EXPECT_EQ(interrupted.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

		reopenNativeStores();
		MetadataSnapshotPin recovered = metadata_->openSnapshot();
		GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
		if (!shouldPublish) {
			EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
			EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edgeId)), FormatException);
			EXPECT_TRUE(scan(adjacency, recovered, GlobalAdjacencyDirection::GAD_OUT, source).empty());
			EXPECT_TRUE(scan(adjacency, recovered, GlobalAdjacencyDirection::GAD_IN, target).empty());
		} else {
			EXPECT_TRUE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
			EXPECT_EQ(entities_->readEdge(recovered, edgeId), (EdgeEntityRecord{source, target, 51U}));
			EXPECT_EQ(scan(adjacency, recovered, GlobalAdjacencyDirection::GAD_OUT, source),
					  (std::vector<uint64_t>{edgeId}));
			EXPECT_EQ(scan(adjacency, recovered, GlobalAdjacencyDirection::GAD_IN, target),
					  (std::vector<uint64_t>{edgeId}));
		}
		recovered.release();
	};

	assertRecovered(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE, false);
	assertRecovered(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, false);
	assertRecovered(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE, true);
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, ResumesRangesWithLogicalContinuationOnly) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t thirdTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t first = transaction.createEdge(source, firstTarget, 7U);
	const uint64_t second = transaction.createEdge(source, secondTarget, 7U);
	const uint64_t third = transaction.createEdge(source, thirdTarget, 7U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	const GlobalAdjacencyRange firstPage = adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 2U);
	EXPECT_EQ(firstPage.edge_ids, (std::vector<uint64_t>{first, second}));
	ASSERT_TRUE(firstPage.continuation.has_value());
	EXPECT_EQ(firstPage.continuation->last_edge_id, second);
	const GlobalAdjacencyRange secondPage =
			adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 2U, firstPage.continuation);
	EXPECT_EQ(secondPage.edge_ids, (std::vector<uint64_t>{third}));
	EXPECT_FALSE(secondPage.continuation.has_value());

	GlobalAdjacencyContinuation foreign = *firstPage.continuation;
	foreign.node_id = firstTarget;
	EXPECT_THROW(static_cast<void>(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 2U, foreign)),
				 FormatException);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, RelocatesTheGlobalTreeWithoutChangingLogicalAdjacencyRouting) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t first = transaction.createEdge(source, firstTarget, 7U);
	const uint64_t second = transaction.createEdge(source, secondTarget, 9U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
	const IndexRootLocation sourceRoot = metadata_->findIndexRoot(sourceSnapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	const PageLocation sourceLocation = metadata_->findPageLocation(sourceSnapshot, sourceRoot.root_page_id);
	const CatalogEntry sourcePool = metadata_->findCatalog(sourceSnapshot, sourceLocation.segment_id);
	sourceSnapshot.release();

	// A separately rooted empty tree makes the destination a live Index pool,
	// rather than introducing an orphan frame merely to reserve compaction space.
	constexpr uint64_t SUPPORT_INDEX_ID = 17U;
	const SlottedPagePoolAllocationRequest destinationRequest =
			indexPoolRequest(SegmentId(900U), PageId(900U), Generation(2U), SUPPORT_INDEX_ID);
	const LogSequenceNumber destinationLsn = nextCheckpointLsn(*metadata_);
	ASSERT_TRUE(btree_pages_
						->allocate(destinationRequest, emptyIndexLeaf(destinationRequest, destinationLsn),
								   PhysicalSlotId(90U), PhysicalSlotId(90U), destinationLsn)
						.has_value());
	const IndexRootLocation supportRoot{SUPPORT_INDEX_ID, destinationRequest.page_id, destinationRequest.generation};
	static_cast<void>(btree_pages_->publishRoots(std::span(&supportRoot, 1U), {}, nextCheckpointLsn(*metadata_)));

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scan(adjacency, oldSnapshot, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{first, second}));

	RelocatableCompactionCoordinator coordinator(*slotted_pages_, *btree_pages_, *metadata_);
	const RelocatableSlottedPoolCompactionResult compacted = coordinator.compactBTreePool(
			{sourcePool.segment_id, destinationRequest.segment_id, 1U, nextCheckpointLsn(*metadata_)});
	EXPECT_EQ(compacted.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(compacted.moved_pages, 1U);
	EXPECT_TRUE(compacted.source_empty);
	EXPECT_TRUE(compacted.source_retired);
	EXPECT_FALSE(compacted.source_released);

	MetadataSnapshotPin currentSnapshot = metadata_->openSnapshot();
	const IndexRootLocation relocatedRoot = metadata_->findIndexRoot(currentSnapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	EXPECT_EQ(relocatedRoot.index_id, sourceRoot.index_id);
	EXPECT_EQ(relocatedRoot.root_page_id, sourceRoot.root_page_id);
	EXPECT_EQ(relocatedRoot.root_generation, destinationRequest.generation);
	EXPECT_EQ(metadata_->findPageLocation(currentSnapshot, sourceRoot.root_page_id).segment_id,
			  destinationRequest.segment_id);
	EXPECT_EQ(scan(adjacency, currentSnapshot, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{first, second}));
	EXPECT_EQ(scan(adjacency, oldSnapshot, GlobalAdjacencyDirection::GAD_OUT, source),
			  (std::vector<uint64_t>{first, second}));
	currentSnapshot.release();

	EXPECT_FALSE(
			coordinator.releaseRetiredSlottedPool(sourcePool.segment_id, sourcePool.generation, compacted.next_lsn));
	oldSnapshot.release();
	EXPECT_TRUE(
			coordinator.releaseRetiredSlottedPool(sourcePool.segment_id, sourcePool.generation, compacted.next_lsn));
	MetadataSnapshotPin releasedSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(releasedSnapshot, sourcePool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(releasedSnapshot, sourcePool.physical_slot));
	releasedSnapshot.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, StreamsASupernodeAcrossMultipleLeavesWithoutSkippingOrDuplicatingEdges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(noLabels);
	std::array<uint64_t, 17U> targets{};
	for (uint64_t &target: targets) {
		target = transaction.createNode(noLabels);
	}

	std::vector<uint64_t> expectedOutgoing;
	std::array<std::vector<uint64_t>, 17U> expectedIncoming;
	const size_t edgeCount = edgesRequiredToSplitOneAdjacencyLeaf();
	for (size_t ordinal = 0U; ordinal < edgeCount; ++ordinal) {
		const size_t targetOrdinal = ordinal % targets.size();
		const uint64_t edgeId = transaction.createEdge(source, targets[targetOrdinal], 1'000U + ordinal);
		expectedOutgoing.push_back(edgeId);
		expectedIncoming[targetOrdinal].push_back(edgeId);
	}
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const IndexRootLocation root = metadata_->findIndexRoot(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	EXPECT_EQ(btree_pages_->read(snapshot, root.root_page_id).header().kind, BTreeNodeKind::BPN_INTERNAL);
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	const GlobalAdjacencyRange firstCursorPage =
			adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 13U);
	ASSERT_EQ(firstCursorPage.edge_ids,
			  (std::vector<uint64_t>(expectedOutgoing.begin(), expectedOutgoing.begin() + 13U)));
	ASSERT_TRUE(firstCursorPage.continuation.has_value());
	EXPECT_EQ(scanAll(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, source, expectedOutgoing.size()),
			  expectedOutgoing);
	for (size_t ordinal = 0U; ordinal < targets.size(); ++ordinal) {
		EXPECT_EQ(scanAll(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, targets[ordinal],
						  expectedIncoming[ordinal].size()),
				  expectedIncoming[ordinal]);
	}

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseEdge(expectedOutgoing.front());
	ASSERT_TRUE(erase.commit().committed());
	const GlobalAdjacencyRange resumedOldCursor = adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source,
																 expectedOutgoing.size(), firstCursorPage.continuation);
	EXPECT_EQ(resumedOldCursor.edge_ids,
			  (std::vector<uint64_t>(expectedOutgoing.begin() + 13U, expectedOutgoing.end())));
	EXPECT_FALSE(resumedOldCursor.continuation.has_value());

	std::vector<uint64_t> expectedCurrentOutgoing(expectedOutgoing.begin() + 1U, expectedOutgoing.end());
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(scanAll(adjacency, current, GlobalAdjacencyDirection::GAD_OUT, source, expectedCurrentOutgoing.size()),
			  expectedCurrentOutgoing);
	current.release();
	snapshot.release();

	reopenNativeStores();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	GlobalAdjacencyIndex reopenedAdjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(scanAll(reopenedAdjacency, reopened, GlobalAdjacencyDirection::GAD_OUT, source,
					  expectedCurrentOutgoing.size()),
			  expectedCurrentOutgoing);
	reopened.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, MaintainsTheGlobalTreeAcrossRandomizedCreateDeleteBatches) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWriteTransaction createNodes = planner.beginTransaction();
	std::array<uint64_t, 12U> nodes{};
	for (uint64_t &nodeId: nodes) {
		nodeId = createNodes.createNode(noLabels);
	}
	ASSERT_TRUE(createNodes.commit().committed());

	struct ExpectedEdge {
		uint64_t source = 0U;
		uint64_t target = 0U;
		uint64_t type = 0U;
	};
	std::map<uint64_t, ExpectedEdge> expected;
	uint64_t state = 0xC6BC279692B5CC83ULL;
	const auto next = [&state]() {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return state;
	};

	for (size_t batch = 0U; batch < 48U; ++batch) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		if (!expected.empty() && next() % 3U == 0U) {
			const size_t ordinal = static_cast<size_t>(next() % expected.size());
			auto edge = expected.begin();
			std::advance(edge, static_cast<std::ptrdiff_t>(ordinal));
			transaction.eraseEdge(edge->first);
			expected.erase(edge);
		}
		const size_t additions = 1U + static_cast<size_t>(next() % 4U);
		for (size_t addition = 0U; addition < additions; ++addition) {
			const uint64_t source = nodes[static_cast<size_t>(next() % nodes.size())];
			const uint64_t target = nodes[static_cast<size_t>(next() % nodes.size())];
			const uint64_t type = 2'000U + batch;
			const uint64_t edgeId = transaction.createEdge(source, target, type);
			expected.emplace(edgeId, ExpectedEdge{source, target, type});
		}
		ASSERT_TRUE(transaction.commit().committed());
	}

	std::map<uint64_t, std::vector<uint64_t>> expectedOutgoing;
	std::map<uint64_t, std::vector<uint64_t>> expectedIncoming;
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	for (const auto &[edgeId, edge]: expected) {
		expectedOutgoing[edge.source].push_back(edgeId);
		expectedIncoming[edge.target].push_back(edgeId);
		EXPECT_EQ(entities_->readEdge(snapshot, edgeId), (EdgeEntityRecord{edge.source, edge.target, edge.type}));
	}
	for (const uint64_t nodeId: nodes) {
		EXPECT_EQ(scanAll(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, nodeId,
						  expectedOutgoing[nodeId].size()),
				  expectedOutgoing[nodeId]);
		EXPECT_EQ(
				scanAll(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, nodeId, expectedIncoming[nodeId].size()),
				expectedIncoming[nodeId]);
	}
	snapshot.release();

	reopenNativeStores();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	GlobalAdjacencyIndex reopenedAdjacency(*btree_pages_, *metadata_);
	for (const uint64_t nodeId: nodes) {
		EXPECT_EQ(scanAll(reopenedAdjacency, reopened, GlobalAdjacencyDirection::GAD_OUT, nodeId,
						  expectedOutgoing[nodeId].size()),
				  expectedOutgoing[nodeId]);
		EXPECT_EQ(scanAll(reopenedAdjacency, reopened, GlobalAdjacencyDirection::GAD_IN, nodeId,
						  expectedIncoming[nodeId].size()),
				  expectedIncoming[nodeId]);
	}
	reopened.release();
}

TEST_F(NativeGraphWritePlannerAdjacencyTest, MaintainsTheGlobalTreeAcrossDeterministicCreateDeleteBatches) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWriteTransaction createNodes = planner.beginTransaction();
	std::array<uint64_t, 6U> nodes{};
	for (uint64_t &nodeId: nodes) {
		nodeId = createNodes.createNode(noLabels);
	}
	ASSERT_TRUE(createNodes.commit().committed());

	struct ExpectedEdge {
		uint64_t source = 0U;
		uint64_t target = 0U;
		uint64_t type = 0U;
	};
	std::map<uint64_t, ExpectedEdge> expected;
	uint64_t state = 0x9E3779B97F4A7C15ULL;
	const auto next = [&state]() {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return state;
	};

	for (size_t batch = 0U; batch < 24U; ++batch) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		if (!expected.empty() && next() % 3U == 0U) {
			const size_t ordinal = static_cast<size_t>(next() % expected.size());
			auto edge = expected.begin();
			std::advance(edge, static_cast<std::ptrdiff_t>(ordinal));
			transaction.eraseEdge(edge->first);
			expected.erase(edge);
		}
		const size_t additions = 1U + static_cast<size_t>(next() % 3U);
		for (size_t addition = 0U; addition < additions; ++addition) {
			const uint64_t source = nodes[static_cast<size_t>(next() % nodes.size())];
			const uint64_t target = nodes[static_cast<size_t>(next() % nodes.size())];
			const uint64_t type = 100U + batch;
			const uint64_t edgeId = transaction.createEdge(source, target, type);
			expected.emplace(edgeId, ExpectedEdge{source, target, type});
		}
		ASSERT_TRUE(transaction.commit().committed());

		std::map<uint64_t, std::vector<uint64_t>> expectedOutgoing;
		std::map<uint64_t, std::vector<uint64_t>> expectedIncoming;
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
		for (const auto &[edgeId, edge]: expected) {
			expectedOutgoing[edge.source].push_back(edgeId);
			expectedIncoming[edge.target].push_back(edgeId);
			EXPECT_EQ(entities_->readEdge(snapshot, edgeId), (EdgeEntityRecord{edge.source, edge.target, edge.type}));
		}
		for (const uint64_t nodeId: nodes) {
			EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_OUT, nodeId), expectedOutgoing[nodeId]);
			EXPECT_EQ(scan(adjacency, snapshot, GlobalAdjacencyDirection::GAD_IN, nodeId), expectedIncoming[nodeId]);
		}
		snapshot.release();
	}
}
