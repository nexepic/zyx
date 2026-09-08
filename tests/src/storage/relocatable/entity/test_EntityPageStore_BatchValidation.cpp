/**
 * @file test_EntityPageStore_BatchValidation.cpp
 * @brief Batch append, COW validation, and atomic retirement tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/entity/EntityPageStoreTestFixture.hpp"

TEST_F(RelocatableEntityPageStoreTest, BatchAppendCreatesMultiRecordNodePageAndRejectsMalformedPlans) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> nodePool =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	const std::optional<EntityPageInsertResult> edgePool =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, edge(1001U, 1002U, 21U), LogSequenceNumber(3U));
	ASSERT_TRUE(nodePool.has_value());
	ASSERT_TRUE(edgePool.has_value());
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();

	const std::array<EntityPageBatchAppend, 1U> valid{{
			{.request = {nodePool->pool, PageId(102U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(31U, 32U)}, {1002U, node(21U, 22U)}}},
	}};
	const EntityPageBatchResult inserted = store.applyBatchWithAppends(snapshot, {}, valid, LogSequenceNumber(4U));
	ASSERT_TRUE(inserted.pages.empty());
	ASSERT_EQ(inserted.appended_pages.size(), 1U);
	ASSERT_EQ(inserted.appended_pages.front().locations.size(), 2U);
	EXPECT_EQ(inserted.appended_pages.front().locations[0], (EntityPageLocation{1002U, PageId(102U), 1U, 1U}));
	EXPECT_EQ(inserted.appended_pages.front().locations[1], (EntityPageLocation{1003U, PageId(102U), 0U, 1U}));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1002U), valid.front().node_insertions[1].record);
	EXPECT_EQ(store.readNode(current, 1003U), valid.front().node_insertions[0].record);

	const auto expectAppendError = [&](const EntityPageBatchAppend &append, FormatError expected) {
		expectFormatError(
				[&] {
					static_cast<void>(
							store.applyBatchWithAppends(current, {}, std::span(&append, 1), LogSequenceNumber(5U)));
				},
				expected);
	};
	EntityPageBatchAppend invalidPage{.request = {nodePool->pool, PageId{}},
									  .record_kind = EntityPageRecordKind::EPRK_NODE,
									  .node_insertions = {{1004U, node(41U, 42U)}}};
	expectAppendError(invalidPage, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend invalidKind{.request = {nodePool->pool, PageId(103U)},
									  .record_kind = static_cast<EntityPageRecordKind>(77U),
									  .node_insertions = {{1004U, node(41U, 42U)}}};
	expectAppendError(invalidKind, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend duplicatePage{.request = {nodePool->pool, PageId(101U)},
										.record_kind = EntityPageRecordKind::EPRK_NODE,
										.node_insertions = {{1004U, node(41U, 42U)}}};
	expectAppendError(duplicatePage, FormatError::FERR_INVALID_STATE);
	EntityPageBatchAppend emptyNode{.request = {nodePool->pool, PageId(103U)},
									.record_kind = EntityPageRecordKind::EPRK_NODE};
	expectAppendError(emptyNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend mixedKinds{.request = {nodePool->pool, PageId(103U)},
									 .record_kind = EntityPageRecordKind::EPRK_NODE,
									 .node_insertions = {{1004U, node(41U, 42U)}},
									 .edge_insertions = {{2002U, edge(1001U, 1002U, 22U)}}};
	expectAppendError(mixedKinds, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend zeroNode{.request = {nodePool->pool, PageId(103U)},
								   .record_kind = EntityPageRecordKind::EPRK_NODE,
								   .node_insertions = {{1005U, node(41U, 42U)}, {0U, node(51U, 52U)}}};
	expectAppendError(zeroNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend duplicateNode{.request = {nodePool->pool, PageId(103U)},
										.record_kind = EntityPageRecordKind::EPRK_NODE,
										.node_insertions = {{1004U, node(41U, 42U)}, {1004U, node(51U, 52U)}}};
	expectAppendError(duplicateNode, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend publishedNode{.request = {nodePool->pool, PageId(103U)},
										.record_kind = EntityPageRecordKind::EPRK_NODE,
										.node_insertions = {{1001U, node(41U, 42U)}}};
	expectAppendError(publishedNode, FormatError::FERR_INVALID_STATE);
	EntityPageBatchAppend zeroEdge{
			.request = {edgePool->pool, PageId(202U)},
			.record_kind = EntityPageRecordKind::EPRK_EDGE,
			.edge_insertions = {{2003U, edge(1001U, 1002U, 22U)}, {0U, edge(1002U, 1001U, 23U)}}};
	expectAppendError(zeroEdge, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend duplicateEdge{
			.request = {edgePool->pool, PageId(202U)},
			.record_kind = EntityPageRecordKind::EPRK_EDGE,
			.edge_insertions = {{2002U, edge(1001U, 1002U, 22U)}, {2002U, edge(1002U, 1001U, 23U)}}};
	expectAppendError(duplicateEdge, FormatError::FERR_INVALID_HEADER);
	EntityPageBatchAppend publishedEdge{.request = {edgePool->pool, PageId(202U)},
										.record_kind = EntityPageRecordKind::EPRK_EDGE,
										.edge_insertions = {{2001U, edge(1001U, 1002U, 22U)}}};
	expectAppendError(publishedEdge, FormatError::FERR_INVALID_STATE);
	EntityPageBatchAppend stalePool{.request = {nodePool->pool, PageId(103U)},
									.record_kind = EntityPageRecordKind::EPRK_NODE,
									.node_insertions = {{1004U, node(41U, 42U)}}};
	stalePool.request.expected_pool.owner_id += 1U;
	expectAppendError(stalePool, FormatError::FERR_INVALID_STATE);
	const std::array<EntityPageBatchAppend, 2U> duplicateBatchPage{{
			{.request = {nodePool->pool, PageId(103U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1004U, node(41U, 42U)}}},
			{.request = {nodePool->pool, PageId(103U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1005U, node(51U, 52U)}}},
	}};
	expectFormatError(
			[&] {
				static_cast<void>(store.applyBatchWithAppends(current, {}, duplicateBatchPage, LogSequenceNumber(5U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(103U)).has_value());
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1004U).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowFailpointLeavesAllPagesAndMapsAtThePriorGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalNode = node(11U, 12U);
	const EdgeEntityRecord originalEdge = edge(1001U, 1002U, 21U);
	ASSERT_TRUE(
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalNode, LogSequenceNumber(2U)).has_value());
	ASSERT_TRUE(
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, originalEdge, LogSequenceNumber(3U)).has_value());
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 2> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(81U, 82U, 37U)}},
			 .node_insertions = {{1002U, node(91U, 92U, 47U)}}},
			{.page_id = PageId(201U),
			 .record_kind = EntityPageRecordKind::EPRK_EDGE,
			 .edge_updates = {{2001U, edge(1001U, 1003U, 31U)}},
			 .edge_insertions = {{2002U, edge(1002U, 1001U, 32U)}}},
	}};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.applyBatch(before, mutations, LogSequenceNumber(4U), interrupted)),
				 BootstrapPublishInterrupted);

	const MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_EQ(afterFailure.superblock(), before.superblock());
	EXPECT_EQ(store.readNode(afterFailure, 1001U), originalNode);
	EXPECT_EQ(store.readEdge(afterFailure, 2001U), originalEdge);
	expectFormatError([&] { static_cast<void>(store.readNode(afterFailure, 1002U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readEdge(afterFailure, 2002U)); }, FormatError::FERR_INVALID_STATE);

	const EntityPageBatchResult retried = store.applyBatch(afterFailure, mutations, LogSequenceNumber(5U));
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock(), retried.superblock);
	EXPECT_EQ(store.readNode(current, 1001U), mutations[0].node_updates.front().record);
	EXPECT_EQ(store.readNode(current, 1002U), mutations[0].node_insertions.front().record);
	EXPECT_EQ(store.readEdge(current, 2001U), mutations[1].edge_updates.front().record);
	EXPECT_EQ(store.readEdge(current, 2002U), mutations[1].edge_insertions.front().record);
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowRejectsInvalidOrUnretiredPagePlansWithoutPublishing) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	const EntityPageInsertResult second =
			store.appendNodePage({first->pool, PageId(102U)}, 1002U, node(21U, 22U, 17U), LogSequenceNumber(3U));
	const std::optional<EntityPageInsertResult> firstEdge =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, edge(10U, 11U, 21U), LogSequenceNumber(4U));
	ASSERT_TRUE(firstEdge.has_value());
	const EntityPageInsertResult secondEdge =
			store.appendEdgePage({firstEdge->pool, PageId(202U)}, 2002U, edge(12U, 13U, 22U), LogSequenceNumber(5U));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const auto expectBatchError = [&](const EntityPageBatchMutation &mutation, FormatError expected) {
		expectFormatError(
				[&] { static_cast<void>(store.applyBatch(snapshot, std::span(&mutation, 1), LogSequenceNumber(6U))); },
				expected);
	};

	expectFormatError([&] { static_cast<void>(store.applyBatch(snapshot, {}, LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation validUpdate{.page_id = PageId(101U),
											  .record_kind = EntityPageRecordKind::EPRK_NODE,
											  .node_updates = {{1001U, node(31U, 32U, 27U)}}};
	expectFormatError([&] { static_cast<void>(store.applyBatch(snapshot, std::span(&validUpdate, 1), {})); },
					  FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation invalidPageId{.record_kind = EntityPageRecordKind::EPRK_NODE,
												.node_updates = {{1001U, node(31U, 32U, 27U)}}};
	expectBatchError(invalidPageId, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation invalidRecordKind{.page_id = PageId(101U),
													.record_kind = static_cast<EntityPageRecordKind>(77U),
													.node_updates = {{1001U, node(31U, 32U, 27U)}}};
	expectBatchError(invalidRecordKind, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation emptyMutation{.page_id = PageId(101U),
												.record_kind = EntityPageRecordKind::EPRK_NODE};
	expectBatchError(emptyMutation, FormatError::FERR_INVALID_HEADER);
	const std::array<EntityPageBatchMutation, 2> duplicatePageIds{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, node(31U, 32U, 27U)}}},
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(41U, 42U, 37U)}}},
	}};
	expectFormatError([&] { static_cast<void>(store.applyBatch(snapshot, duplicatePageIds, LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation mixedKinds{.page_id = PageId(101U),
											 .record_kind = EntityPageRecordKind::EPRK_NODE,
											 .edge_updates = {{2001U, edge(10U, 11U, 21U)}}};
	expectFormatError(
			[&] { static_cast<void>(store.applyBatch(snapshot, std::span(&mixedKinds, 1), LogSequenceNumber(4U))); },
			FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation foreignNode{.page_id = PageId(102U),
											  .record_kind = EntityPageRecordKind::EPRK_NODE,
											  .node_updates = {{1001U, node(31U, 32U, 27U)}}};
	expectFormatError(
			[&] { static_cast<void>(store.applyBatch(snapshot, std::span(&foreignNode, 1), LogSequenceNumber(4U))); },
			FormatError::FERR_INVALID_STATE);
	const EntityPageBatchMutation zeroId{.page_id = PageId(101U),
										 .record_kind = EntityPageRecordKind::EPRK_NODE,
										 .node_updates = {{0U, node(31U, 32U, 27U)}}};
	expectFormatError(
			[&] { static_cast<void>(store.applyBatch(snapshot, std::span(&zeroId, 1), LogSequenceNumber(4U))); },
			FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation duplicateNodeId{.page_id = PageId(101U),
												  .record_kind = EntityPageRecordKind::EPRK_NODE,
												  .node_updates = {{1001U, node(31U, 32U, 27U)}},
												  .node_insertions = {{1001U, node(41U, 42U, 37U)}}};
	expectBatchError(duplicateNodeId, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation zeroNodeInsertion{.page_id = PageId(101U),
													.record_kind = EntityPageRecordKind::EPRK_NODE,
													.node_insertions = {{0U, node(31U, 32U, 27U)}}};
	expectBatchError(zeroNodeInsertion, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation publishedNodeInsertion{.page_id = PageId(101U),
														 .record_kind = EntityPageRecordKind::EPRK_NODE,
														 .node_insertions = {{1001U, node(31U, 32U, 27U)}}};
	expectBatchError(publishedNodeInsertion, FormatError::FERR_INVALID_STATE);
	const EntityPageBatchMutation zeroNodeErase{
			.page_id = PageId(101U), .record_kind = EntityPageRecordKind::EPRK_NODE, .erased_node_ids = {0U}};
	expectBatchError(zeroNodeErase, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation duplicateNodeErase{
			.page_id = PageId(101U), .record_kind = EntityPageRecordKind::EPRK_NODE, .erased_node_ids = {1001U, 1001U}};
	expectBatchError(duplicateNodeErase, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation foreignNodeErase{
			.page_id = PageId(102U), .record_kind = EntityPageRecordKind::EPRK_NODE, .erased_node_ids = {1001U}};
	expectBatchError(foreignNodeErase, FormatError::FERR_INVALID_STATE);
	const std::array<EntityPageBatchMutation, 2> duplicateInsertions{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(31U, 32U, 27U)}}},
			{.page_id = PageId(102U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1003U, node(41U, 42U, 37U)}}},
	}};
	expectFormatError(
			[&] { static_cast<void>(store.applyBatch(snapshot, duplicateInsertions, LogSequenceNumber(4U))); },
			FormatError::FERR_INVALID_STATE);
	const EntityPageBatchMutation zeroEdgeUpdate{.page_id = PageId(201U),
												 .record_kind = EntityPageRecordKind::EPRK_EDGE,
												 .edge_updates = {{0U, edge(10U, 11U, 21U)}}};
	expectBatchError(zeroEdgeUpdate, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation foreignEdgeUpdate{.page_id = PageId(202U),
													.record_kind = EntityPageRecordKind::EPRK_EDGE,
													.edge_updates = {{2001U, edge(10U, 11U, 21U)}}};
	expectBatchError(foreignEdgeUpdate, FormatError::FERR_INVALID_STATE);
	const EntityPageBatchMutation zeroEdgeInsertion{.page_id = PageId(201U),
													.record_kind = EntityPageRecordKind::EPRK_EDGE,
													.edge_insertions = {{0U, edge(10U, 11U, 21U)}}};
	expectBatchError(zeroEdgeInsertion, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation publishedEdgeInsertion{.page_id = PageId(201U),
														 .record_kind = EntityPageRecordKind::EPRK_EDGE,
														 .edge_insertions = {{2001U, edge(10U, 11U, 21U)}}};
	expectBatchError(publishedEdgeInsertion, FormatError::FERR_INVALID_STATE);
	const EntityPageBatchMutation zeroEdgeErase{
			.page_id = PageId(201U), .record_kind = EntityPageRecordKind::EPRK_EDGE, .erased_edge_ids = {0U}};
	expectBatchError(zeroEdgeErase, FormatError::FERR_INVALID_HEADER);
	const EntityPageBatchMutation foreignEdgeErase{
			.page_id = PageId(202U), .record_kind = EntityPageRecordKind::EPRK_EDGE, .erased_edge_ids = {2001U}};
	expectBatchError(foreignEdgeErase, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.readNode(snapshot, 1001U), firstNode);
	EXPECT_EQ(store.readNode(snapshot, 1002U), node(21U, 22U, 17U));
	EXPECT_FALSE(metadata->findNodePageLocationOptional(snapshot, 1003U).has_value());
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(102U)), second.page);
	EXPECT_EQ(store.readEdge(snapshot, 2001U), edge(10U, 11U, 21U));
	EXPECT_EQ(store.readEdge(snapshot, 2002U), edge(12U, 13U, 22U));
	EXPECT_EQ(metadata->findPageLocation(snapshot, PageId(202U)), secondEdge.page);
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowErasesNodeAndEdgeMappingsWithoutRetiringPartiallyLivePages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord firstNode = node(11U, 12U);
	const std::optional<EntityPageInsertResult> nodePage =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, firstNode, LogSequenceNumber(2U));
	ASSERT_TRUE(nodePage.has_value());
	const MetadataSnapshotPin afterFirstNode = metadata->openSnapshot();
	const NodeEntityRecord survivingNode = node(21U, 22U, 17U);
	static_cast<void>(store.insertNode(afterFirstNode, PageId(101U), 1002U, survivingNode, LogSequenceNumber(3U)));
	const EdgeEntityRecord firstEdge = edge(1001U, 1002U, 21U);
	const std::optional<EntityPageInsertResult> edgePage =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, firstEdge, LogSequenceNumber(4U));
	ASSERT_TRUE(edgePage.has_value());
	const MetadataSnapshotPin afterFirstEdge = metadata->openSnapshot();
	const EdgeEntityRecord survivingEdge = edge(1002U, 1001U, 22U);
	static_cast<void>(store.insertEdge(afterFirstEdge, PageId(201U), 2002U, survivingEdge, LogSequenceNumber(5U)));
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 2> mutations{{
			{.page_id = PageId(101U), .record_kind = EntityPageRecordKind::EPRK_NODE, .erased_node_ids = {1001U}},
			{.page_id = PageId(201U), .record_kind = EntityPageRecordKind::EPRK_EDGE, .erased_edge_ids = {2001U}},
	}};
	const EntityPageBatchResult result = store.applyBatch(before, mutations, LogSequenceNumber(6U));
	ASSERT_EQ(result.pages.size(), mutations.size());
	EXPECT_EQ(store.readNode(before, 1001U), firstNode);
	EXPECT_EQ(store.readEdge(before, 2001U), firstEdge);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1001U).has_value());
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(current, 2001U).has_value());
	EXPECT_EQ(store.readNode(current, 1002U), survivingNode);
	EXPECT_EQ(store.readEdge(current, 2002U), survivingEdge);
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(101U)).has_value());
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(201U)).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowRetiresFinalNodeAndEdgePagesWithoutPartialPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord originalNode = node(11U, 12U);
	const EdgeEntityRecord originalEdge = edge(1001U, 1002U, 21U);
	ASSERT_TRUE(
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, originalNode, LogSequenceNumber(2U)).has_value());
	ASSERT_TRUE(
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, originalEdge, LogSequenceNumber(3U)).has_value());
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchMutation, 2> mutations{{
			{.page_id = PageId(101U), .record_kind = EntityPageRecordKind::EPRK_NODE, .erased_node_ids = {1001U}},
			{.page_id = PageId(201U), .record_kind = EntityPageRecordKind::EPRK_EDGE, .erased_edge_ids = {2001U}},
	}};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.applyBatch(before, mutations, LogSequenceNumber(4U), interrupted)),
				 BootstrapPublishInterrupted);
	const MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(afterFailure, 1001U), originalNode);
	EXPECT_EQ(store.readEdge(afterFailure, 2001U), originalEdge);
	EXPECT_TRUE(metadata->findPageLocationOptional(afterFailure, PageId(101U)).has_value());
	EXPECT_TRUE(metadata->findPageLocationOptional(afterFailure, PageId(201U)).has_value());

	const EntityPageBatchResult result = store.applyBatch(afterFailure, mutations, LogSequenceNumber(5U));
	ASSERT_EQ(result.pages.size(), mutations.size());
	EXPECT_TRUE(result.pages[0].page_retired);
	EXPECT_TRUE(result.pages[1].page_retired);
	EXPECT_EQ(result.pages[0].page.page_id, PageId(101U));
	EXPECT_EQ(result.pages[1].page.page_id, PageId(201U));
	EXPECT_EQ(store.readNode(before, 1001U), originalNode);
	EXPECT_EQ(store.readEdge(before, 2001U), originalEdge);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findNodePageLocationOptional(current, 1001U).has_value());
	EXPECT_FALSE(metadata->findEdgePageLocationOptional(current, 2001U).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(101U)).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(201U)).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, BatchCowRequiresOneConsistentIdentityForSharedFreshDestinationPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(11U, 12U), LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	static_cast<void>(store.appendNodePage(
			{first->pool, PageId(102U)}, 1002U, node(21U, 22U), LogSequenceNumber(3U)));
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const uintmax_t initialFileBytes = std::filesystem::file_size(path);

	const EntityPageFreshPoolDestination destination{
			SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(42U)};
	const auto mutationsFor = [&](const EntityPageFreshPoolDestination &secondDestination) {
		return std::array<EntityPageBatchMutation, 2U>{
				EntityPageBatchMutation{.page_id = PageId(101U),
								.record_kind = EntityPageRecordKind::EPRK_NODE,
								.node_updates = {{1001U, node(31U, 32U)}},
								.replacement_new_pool = destination},
				EntityPageBatchMutation{.page_id = PageId(102U),
								.record_kind = EntityPageRecordKind::EPRK_NODE,
								.node_updates = {{1002U, node(41U, 42U)}},
								.replacement_new_pool = secondDestination}};
	};
	const auto expectIdentityConflict = [&](EntityPageFreshPoolDestination conflicting) {
		const auto mutations = mutationsFor(conflicting);
		expectFormatError(
				[&] { static_cast<void>(store.applyBatch(before, mutations, LogSequenceNumber(4U))); },
				FormatError::FERR_INVALID_STATE);
		EXPECT_EQ(std::filesystem::file_size(path), initialFileBytes);
		const MetadataSnapshotPin unchanged = metadata->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), before.superblock());
		EXPECT_EQ(store.currentPools(unchanged).size(), 1U);
		EXPECT_EQ(metadata->findPageLocation(unchanged, PageId(101U)).segment_id, SegmentId(31U));
		EXPECT_EQ(metadata->findPageLocation(unchanged, PageId(102U)).segment_id, SegmentId(31U));
	};

	EntityPageFreshPoolDestination conflicting = destination;
	conflicting.pool_owner_id += 1U;
	expectIdentityConflict(conflicting);
	conflicting = destination;
	conflicting.generation = Generation(2U);
	expectIdentityConflict(conflicting);
	conflicting = destination;
	conflicting.first_slot = PhysicalSlotId(42U);
	expectIdentityConflict(conflicting);
	conflicting = destination;
	conflicting.last_slot = PhysicalSlotId(43U);
	expectIdentityConflict(conflicting);

	const auto consistent = mutationsFor(destination);
	const EntityPageBatchResult published = store.applyBatch(before, consistent, LogSequenceNumber(4U));
	ASSERT_EQ(published.pages.size(), consistent.size());
	EXPECT_EQ(published.pages[0].pool.segment_id, destination.segment_id);
	EXPECT_EQ(published.pages[1].pool.segment_id, destination.segment_id);
	EXPECT_EQ(published.pages[0].pool.physical_slot, published.pages[1].pool.physical_slot);
	EXPECT_NE(published.pages[0].page.page_index, published.pages[1].page.page_index);

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(101U)).segment_id, destination.segment_id);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102U)).segment_id, destination.segment_id);
	EXPECT_EQ(store.readNode(current, 1001U), consistent[0].node_updates.front().record);
	EXPECT_EQ(store.readNode(current, 1002U), consistent[1].node_updates.front().record);
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsInvalidPropertyPoolOptionsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	LargeValueStore largeValues(io, *metadata);
	PropertyCollectionRootStore properties(pages, largeValues, *metadata);
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	const NativeGraphDerivedStateMutation noDerivedState;
	const NativeGraphDerivedStatePreparer noDerivedStatePreparer;
	const auto expectInvalidOptions = [&](const PropertyCollectionRootStoreOptions &options) {
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, properties, options, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, properties, options, noAuxiliaryPages, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, properties, options, noAuxiliaryPages, noDerivedState,
							noDerivedStatePreparer, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
	};

	PropertyCollectionRootStoreOptions invalidFirstSlot{PhysicalSlotId{}, PhysicalSlotId(48U), 902U};
	expectInvalidOptions(invalidFirstSlot);
	PropertyCollectionRootStoreOptions invertedSlots{PhysicalSlotId(48U), PhysicalSlotId(41U), 902U};
	expectInvalidOptions(invertedSlots);
	PropertyCollectionRootStoreOptions missingOwner{PhysicalSlotId(41U), PhysicalSlotId(48U), 0U};
	expectInvalidOptions(missingOwner);
	const PropertyCollectionRootStoreOptions validOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const EntityPageBatchResult published = store.applyNativeGraphBatchTransaction(
			before, {}, {}, nodePool, {}, properties, validOptions, LogSequenceNumber(2U));
	ASSERT_EQ(published.new_pool_pages.size(), 1U);

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U).page_id, PageId(101U));
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsForeignPropertyRootsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const std::filesystem::path foreignPath = path.string() + ".foreign-property-roots";
	{
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		SlottedPagePool foreignPages(foreignIo, *foreignMetadata);
		PropertyCollectionRootStore foreignProperties(foreignPages, *foreignMetadata);
		const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
		const NativeGraphDerivedStateMutation noDerivedState;
		const NativeGraphDerivedStatePreparer noDerivedStatePreparer;

		// Each public entry point would otherwise publish both Entity and Property roots.
		// A store from another metadata file cannot participate in that atomic generation.
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, foreignProperties, propertyOptions, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(before, {}, {}, nodePool, {}, foreignProperties,
							propertyOptions, noAuxiliaryPages, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, foreignProperties, propertyOptions, noAuxiliaryPages,
							noDerivedState, noDerivedStatePreparer, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsLargeValuePlansWithoutAContentStoreBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	LargeValueStore largeValues(io, *metadata);
	PropertyCollectionRootStore planningProperties(pages, largeValues, *metadata);
	PropertyCollectionRootStore propertiesWithoutContentStore(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const PropertyCollectionRoot largeRoot{
			1001U, graph::Node::typeId,
			{{3U, graph::PropertyValue(std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, 'L'))}}};
	const EntityPropertyRootMutationPlan plan =
			planningProperties.planEntityRootReplacement(before, largeRoot, propertyOptions);
	ASSERT_EQ(plan.large_values.size(), 1U);
	const std::array plans{plan};
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};

	// A caller can carry a precomputed public plan into this API. Reject it
	// before publication when the selected Property store cannot materialize
	// its referenced immutable content.
	expectFormatError(
			[&] {
				static_cast<void>(store.applyNativeGraphBatchTransaction(
						before, {}, {}, nodePool, plans, propertiesWithoutContentStore, propertyOptions,
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(before, plan.key).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsMismatchedDerivedStatePreparersBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	const NativeGraphDerivedStateMutation noDerivedState;
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};
	const NativeGraphVectorBootstrapPreparer bootstrapPreparer =
			[](std::vector<NativeGraphVectorBootstrapMaterialization> &) {};
	const NativeGraphVectorDeltaPreparer deltaPreparer =
			[](std::vector<NativeGraphVectorDeltaMaterialization> &) {};
	const auto expectRejected = [&](const NativeGraphDerivedStateMutation &derived,
									 const NativeGraphDerivedStatePreparer &derivedCallback,
									 const NativeGraphVectorBootstrapPreparer &bootstrapCallback,
									 const NativeGraphVectorDeltaPreparer &deltaCallback) {
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, {}, properties, propertyOptions, noAuxiliaryPages, derived,
							derivedCallback, LogSequenceNumber(2U), {}, {}, bootstrapCallback, deltaCallback));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
	};

	expectRejected(noDerivedState, derivedPreparer, {}, {});
	expectRejected(noDerivedState, {}, bootstrapPreparer, {});
	expectRejected(noDerivedState, {}, {}, deltaPreparer);
	NativeGraphDerivedStateMutation bootstrapState;
	bootstrapState.vector_bootstraps.emplace_back();
	expectRejected(bootstrapState, derivedPreparer, {}, {});
	expectRejected(bootstrapState, derivedPreparer, bootstrapPreparer, deltaPreparer);
	NativeGraphDerivedStateMutation deltaState;
	deltaState.vector_deltas.emplace_back();
	expectRejected(deltaState, derivedPreparer, {}, {});
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsForgedNewPropertyRootPlansBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const PropertyCollectionRoot root{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("native property root")}}};
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan planned =
			properties.planEntityRootReplacement(before, root, propertyOptions);
	ASSERT_EQ(planned.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const auto expectRejected = [&](EntityPropertyRootMutationPlan forged) {
		const std::array plans{std::move(forged)};
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, plans, properties, propertyOptions, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(before, planned.key).has_value());
	};

	EntityPropertyRootMutationPlan invalidKey = planned;
	invalidKey.key = {};
	expectRejected(std::move(invalidKey));
	EntityPropertyRootMutationPlan emptyRoot = planned;
	emptyRoot.encoded_root.clear();
	expectRejected(std::move(emptyRoot));
	EntityPropertyRootMutationPlan oversizedRoot = planned;
	oversizedRoot.encoded_root.resize(
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES + 1U);
	expectRejected(std::move(oversizedRoot));
	EntityPropertyRootMutationPlan unexpectedReplacementPayload = planned;
	unexpectedReplacementPayload.replacement_payload = {std::byte{0x01}};
	expectRejected(std::move(unexpectedReplacementPayload));
	EntityPropertyRootMutationPlan missingRelocatedKey = planned;
	missingRelocatedKey.page_replacement = NativeGraphPropertyPageReplacementRequest{};
	expectRejected(std::move(missingRelocatedKey));
	EntityPropertyRootMutationPlan missingReplacementImage = planned;
	missingReplacementImage.page_replacement = NativeGraphPropertyPageReplacementRequest{};
	missingReplacementImage.page_replacement->relocated_properties.push_back(planned.key);
	expectRejected(std::move(missingReplacementImage));
	EntityPropertyRootMutationPlan malformedRetirement = planned;
	malformedRetirement.page_replacement = NativeGraphPropertyPageReplacementRequest{};
	malformedRetirement.page_replacement->relocated_properties.push_back(planned.key);
	malformedRetirement.page_replacement->retire_page = true;
	expectRejected(std::move(malformedRetirement));

	const std::array validPlans{planned};
	const EntityPageBatchResult published = store.applyNativeGraphBatchTransaction(
			before, {}, {}, nodePool, validPlans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(published.properties.size(), 1U);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(current, planned.key), root);
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchRejectsMalformedExistingPropertyRootPlansBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const PropertyCollectionRoot initialRoot{
			1001U, graph::Node::typeId, {{3U, graph::PropertyValue("existing Property root")}}};
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	MetadataSnapshotPin empty = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan initialPlan =
			properties.planEntityRootReplacement(empty, initialRoot, propertyOptions);
	static_cast<void>(store.applyNativeGraphBatchTransaction(empty, {}, {}, nodePool, std::span(&initialPlan, 1U),
																		 properties, propertyOptions, LogSequenceNumber(2U)));
	empty.release();

	const PropertyCollectionRoot replacementRoot{
			1002U, graph::Node::typeId, {{5U, graph::PropertyValue("replacement Property root")}}};
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const EntityPropertyRootMutationPlan planned =
			properties.planEntityRootReplacement(before, replacementRoot, propertyOptions);
	ASSERT_EQ(planned.kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	ASSERT_TRUE(planned.page_replacement.has_value());
	const auto expectRejected = [&](std::vector<EntityPropertyRootMutationPlan> forged, const char *scenario) {
		SCOPED_TRACE(scenario);
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, {}, forged, properties, propertyOptions, LogSequenceNumber(3U)));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_EQ(properties.readEntity(before, initialPlan.key), initialRoot);
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(before, planned.key).has_value());
	};

	EntityPropertyRootMutationPlan invalidKey = planned;
	invalidKey.key = {};
	expectRejected({std::move(invalidKey)}, "invalid key");
	EntityPropertyRootMutationPlan missingReplacement = planned;
	missingReplacement.page_replacement.reset();
	expectRejected({std::move(missingReplacement)}, "missing replacement");
	EntityPropertyRootMutationPlan unexpectedRoot = planned;
	unexpectedRoot.encoded_root = {std::byte{0x01U}};
	expectRejected({std::move(unexpectedRoot)}, "unexpected encoded root");
	EntityPropertyRootMutationPlan emptyMutation = planned;
	emptyMutation.page_replacement->upserted_properties.clear();
	expectRejected({std::move(emptyMutation)}, "empty mutation");
	EntityPropertyRootMutationPlan unboundMutation = planned;
	unboundMutation.page_replacement->upserted_properties.front().key =
			{EntityPropertyOwnerKind::EPOK_NODE, 1003U};
	expectRejected({std::move(unboundMutation)}, "unbound mutation");
	EntityPropertyRootMutationPlan malformedRetirement = planned;
	malformedRetirement.page_replacement->retire_page = true;
	expectRejected({std::move(malformedRetirement)}, "retirement with upsert");
	EntityPropertyRootMutationPlan retirementWithPayload = planned;
	retirementWithPayload.page_replacement->retire_page = true;
	retirementWithPayload.page_replacement->upserted_properties.clear();
	retirementWithPayload.page_replacement->erased_properties.push_back(planned.key);
	retirementWithPayload.replacement_payload = {std::byte{0x01U}};
	expectRejected({std::move(retirementWithPayload)}, "retirement payload");
	EntityPropertyRootMutationPlan retirementWithFreeBytes = planned;
	retirementWithFreeBytes.page_replacement->retire_page = true;
	retirementWithFreeBytes.page_replacement->upserted_properties.clear();
	retirementWithFreeBytes.page_replacement->erased_properties.push_back(planned.key);
	retirementWithFreeBytes.replacement_free_bytes = 1U;
	expectRejected({std::move(retirementWithFreeBytes)}, "retirement free bytes");
	expectRejected({planned, planned}, "duplicate plan key");
}

TEST_F(RelocatableEntityPageStoreTest, NativeGraphBatchValidatesUnchangedPropertyRootPlansBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions propertyOptions{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(31U, 901U, 101U, 40U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, node(11U, 12U)}}},
	}};
	const EntityPropertyRootMutationPlan unchanged{
			.kind = EntityPropertyRootMutationPlanKind::EPRMPK_NO_CHANGE,
			.key = {EntityPropertyOwnerKind::EPOK_NODE, 1001U},
	};
	const auto expectRejected = [&](EntityPropertyRootMutationPlan forged) {
		const std::array plans{std::move(forged)};
		expectFormatError(
				[&] {
					static_cast<void>(store.applyNativeGraphBatchTransaction(
							before, {}, {}, nodePool, plans, properties, propertyOptions, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(metadata->findNodePageLocationOptional(before, 1001U).has_value());
	};

	EntityPropertyRootMutationPlan invalidKey = unchanged;
	invalidKey.key = {};
	expectRejected(std::move(invalidKey));
	EntityPropertyRootMutationPlan unexpectedReplacement = unchanged;
	unexpectedReplacement.page_replacement = NativeGraphPropertyPageReplacementRequest{};
	expectRejected(std::move(unexpectedReplacement));
	EntityPropertyRootMutationPlan unexpectedPayload = unchanged;
	unexpectedPayload.replacement_payload = {std::byte{0x01}};
	expectRejected(std::move(unexpectedPayload));
	EntityPropertyRootMutationPlan unexpectedRoot = unchanged;
	unexpectedRoot.encoded_root = {std::byte{0x01}};
	expectRejected(std::move(unexpectedRoot));
	EntityPropertyRootMutationPlan unexpectedFreeBytes = unchanged;
	unexpectedFreeBytes.replacement_free_bytes = 1U;
	expectRejected(std::move(unexpectedFreeBytes));

	const std::array plans{unchanged};
	const EntityPageBatchResult published = store.applyNativeGraphBatchTransaction(
			before, {}, {}, nodePool, plans, properties, propertyOptions, LogSequenceNumber(2U));
	ASSERT_EQ(published.new_pool_pages.size(), 1U);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findNodePageLocation(current, 1001U).page_id, PageId(101U));
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(current, unchanged.key).has_value());
}
