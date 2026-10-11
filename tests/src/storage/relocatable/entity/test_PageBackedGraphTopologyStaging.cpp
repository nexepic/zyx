/** @file test_PageBackedGraphTopologyStaging.cpp
 *  @brief Native topology staging with mixed session hits and misses.
 */
#include <array>
#include "graph/debug/PerfTrace.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"
#include "graph/core/Node.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>
#include "storage/relocatable/entity/PageBackedGraphReadAdapterTestFixture.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

using namespace graph::storage;
using graph::storage::test::PageBackedGraphReadAdapterTest;

TEST_F(PageBackedGraphReadAdapterTest, CachedEntityReadsRejectReleasedSnapshotAndAllowLivePinRetry) {
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1U);
	superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata);
	BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U);
	options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U;
	options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const uint64_t id = transaction.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	auto livePin = metadata->openSnapshot();
	NativeReadSession session;
	const auto expected = adapter.readNodeRecord(snapshot, id, &session);
	ASSERT_TRUE(session.node(id).has_value());
	snapshot.release();
	EXPECT_THROW(static_cast<void>(adapter.readNodeRecord(snapshot, id, &session)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(adapter.readNode(snapshot, id, &session)), std::invalid_argument);
	const std::array<uint64_t, 1U> ids{id};
	EXPECT_THROW(static_cast<void>(adapter.readNodeRecordsSorted(snapshot, ids, &session)), std::invalid_argument);
	// A failed lifetime check must not invalidate the still-live generation.
	EXPECT_EQ(adapter.readNodeRecord(livePin, id, &session), expected);
	EXPECT_EQ(adapter.readNode(livePin, id, &session).node_id, id);
	EXPECT_EQ(adapter.readNodeRecordsSorted(livePin, ids, &session), (std::vector<NodeEntityRecord>{expected}));
}

TEST_F(PageBackedGraphReadAdapterTest, TopologyPagePreservesMixedCacheHitsAcrossEvictionAndContinuation) {
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1U);
	superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata);
	BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U);
	options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U;
	options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	constexpr size_t COUNT = NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS + 17U;
	std::vector<uint64_t> expected;
	std::vector<uint64_t> warm;
	for (size_t index = 0U; index < COUNT; ++index) {
		const auto id = transaction.createEdge(source, target, index % 2U == 0U ? 41U : 43U);
		expected.push_back(id);
		if (index % 2U == 0U) warm.push_back(id);
	}
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	NativeReadSession session;
	static_cast<void>(adapter.readEdgeTopologiesSorted(snapshot, warm, &session));
	for (size_t repeat = 0U; repeat < 2U; ++repeat) {
		const auto page = adapter.scanAdjacentEdgeTopologies(snapshot, source,
			PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, COUNT + 1U, &session);
		ASSERT_EQ(page.edge_ids, expected);
		ASSERT_EQ(page.edges.size(), COUNT);
		EXPECT_FALSE(page.continuation.has_value());
		for (size_t index = 0U; index < COUNT; ++index) {
			EXPECT_EQ(page.edges[index], (EdgeEntityRecord{source, target, index % 2U == 0U ? 41U : 43U}));
		}
	}
	size_t fullRetainedBytes = 0U;
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(snapshot, source,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, COUNT + 1U, &session, nullptr,
		[&](size_t bytes) { fullRetainedBytes = bytes; if (bytes != 0U) throw std::length_error("complete buffer rejected"); })), std::length_error);
	EXPECT_EQ(fullRetainedBytes, 16U * sizeof(uint64_t));
	for (size_t retry = 0U; retry < 2U; ++retry) {
		size_t fullPeakBytes = 0U;
		const auto fullPage = adapter.scanAdjacentEdges(snapshot, source,
			PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, COUNT + 1U, &session, nullptr,
			[&](size_t bytes) { fullRetainedBytes = bytes; fullPeakBytes = std::max(fullPeakBytes, bytes); });
		ASSERT_EQ(fullPage.edges.size(), COUNT);
		for (size_t ordinal = 0U; ordinal < COUNT; ++ordinal) EXPECT_EQ(fullPage.edges[ordinal].edge_id, expected[ordinal]);
		EXPECT_EQ(fullRetainedBytes, fullPage.edges.capacity() * sizeof(PageBackedGraphEdge));
		EXPECT_GT(fullPeakBytes, fullRetainedBytes);
		EXPECT_FALSE(fullPage.continuation.has_value());
	}
	size_t retainedBytes = 0U;
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdgeTopologies(snapshot, source,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, COUNT + 1U, &session,
		[&](size_t bytes) { retainedBytes = bytes; if (bytes > 1U) throw std::runtime_error("topology admission rejected"); })),
		std::runtime_error);
	EXPECT_EQ(retainedBytes, 16U * sizeof(uint64_t));
	size_t peakBytes = 0U;
	const auto admitted = adapter.scanAdjacentEdgeTopologies(snapshot, source,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, COUNT + 1U, &session,
		[&](size_t bytes) { retainedBytes = bytes; peakBytes = std::max(peakBytes, bytes); });
	EXPECT_EQ(admitted.edge_ids, expected);
	EXPECT_EQ(retainedBytes, admitted.edge_ids.capacity() * sizeof(uint64_t) + admitted.edges.capacity() * sizeof(EdgeEntityRecord));
	EXPECT_GT(peakBytes, retainedBytes);
	const auto first = adapter.scanAdjacentEdgeTopologies(snapshot, source,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 73U, &session);
	ASSERT_TRUE(first.continuation.has_value());
	const auto second = adapter.scanAdjacentEdgeTopologies(snapshot, source,
		PageBackedGraphReadDirection::PBGRD_OUTGOING, first.continuation, 73U, &session);
	std::vector<uint64_t> combined = first.edge_ids;
	combined.insert(combined.end(), second.edge_ids.begin(), second.edge_ids.end());
	EXPECT_EQ(combined, expected);
	EXPECT_FALSE(second.continuation.has_value());
}

TEST_F(PageBackedGraphReadAdapterTest, BatchAdjacencyAdmissionRejectsBeforeDeliveryAndRetries) {
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1U);
	superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U);
	options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U;
	options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t first = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t second = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t third = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = transaction.createEdge(first, second, 41U);
	const uint64_t secondEdge = transaction.createEdge(second, third, 43U);
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	NativeReadSession session;

	const std::array<uint64_t, 2U> outgoingSources{first, second};
	for (size_t rejectedCall : {size_t{1U}, size_t{5U}}) {
		size_t calls = 0U, delivered = 0U;
		EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgeRefsForNodesUntil(
			snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
			[&](const EntityRef &) { ++delivered; return true; }, &session,
			[&](size_t) { if (++calls == rejectedCall) throw std::runtime_error("source rejected"); })), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
	}
	size_t retained = 0U, peak = 0U;
	std::vector<EntityRef> result;
	EXPECT_TRUE(adapter.visitAdjacentEdgeRefsForNodesUntil(
		snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
		[&](const EntityRef &ref) { result.push_back(ref); return true; }, &session,
		[&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); }));
	EXPECT_EQ(result, (std::vector<EntityRef>{{firstEdge, true, first, second, 41U},
		{secondEdge, true, second, third, 43U}}));
	EXPECT_EQ(retained, 0U);
	EXPECT_GT(peak, 0U);
	size_t delivered = 0U;
	EXPECT_FALSE(adapter.visitAdjacentEdgeRefsForNodesUntil(
		snapshot, outgoingSources, PageBackedGraphReadDirection::PBGRD_OUTGOING, 3U,
		[&](const EntityRef &) { ++delivered; return false; }, &session,
		[&](size_t bytes) { retained = bytes; }));
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(retained, 0U);
}

TEST_F(PageBackedGraphReadAdapterTest, ExactStagingRejectsBeforeSortingAndRetriesUnsortedPredicates) {
	Superblock superblock; superblock.database_id = DatabaseId(99U);
	superblock.generation = Generation(1U); superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata); SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata); BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U); options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U; options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const auto node = transaction.createNode(std::array<uint64_t, 1U>{17U});
	const graph::PropertyValue payload(std::string(128U, 'p'));
	transaction.replaceNodeProperty({node, graph::Node::typeId, {{3U, payload}, {7U, graph::PropertyValue(int64_t{7})}}});
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot(); NativeReadSession session;
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<uint64_t, 3U> repeatedLabels{17U, 17U, 17U};
	const std::array<uint64_t, 2U> missingLabels{17U, 18U};
	const std::array<uint64_t, 2U> zeroLabels{17U, 0U};
	for (int retry = 0; retry < 2; ++retry) {
		EXPECT_TRUE(adapter.nodeHasLabelTokens(snapshot, node, noLabels, &session));
		EXPECT_TRUE(adapter.nodeHasLabelTokens(snapshot, node, repeatedLabels, &session));
		EXPECT_FALSE(adapter.nodeHasLabelTokens(snapshot, node, missingLabels, &session));
		EXPECT_FALSE(adapter.nodeHasLabelTokens(snapshot, node, zeroLabels, &session));
	}
	const std::array ids{node}; const std::array<uint32_t, 2U> tokens{7U, 3U};
	const std::array<graph::PropertyValue, 2U> expected{graph::PropertyValue(int64_t{7}), payload};
	size_t requested = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t bytes) { requested = bytes; throw std::length_error("exact staging rejected"); };
	EXPECT_THROW(adapter.validateNodeExactPropertiesSorted(snapshot, ids, 17U, tokens, expected, 2U, &session, nullptr, &reject), std::length_error);
	EXPECT_EQ(requested, tokens.size() * sizeof(size_t));
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U, peak = 0U;
		const SelectedPropertyReadAdmission accept = [&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); };
		EXPECT_NO_THROW(adapter.validateNodeExactPropertiesSorted(snapshot, ids, 17U, tokens, expected, 2U, &session, nullptr, &accept));
		EXPECT_GT(peak, requested); EXPECT_EQ(retained, 0U);
		EXPECT_EQ(expected[1], payload);
	}
}

TEST_F(PageBackedGraphReadAdapterTest, LabelPredicateBatchMixesInlineOverflowAndUnlabelledRecords) {
	Superblock superblock; superblock.database_id = DatabaseId(101U);
	superblock.generation = Generation(1U); superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata); SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata); BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U); options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U; options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const auto inlineNode = transaction.createNode(std::array<uint64_t, 2U>{3U, 7U});
	const auto overflowNode = transaction.createNode(std::array<uint64_t, 4U>{3U, 7U, 11U, 17U});
	const auto emptyNode = transaction.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot(); NativeReadSession session;
	std::array ids{inlineNode, overflowNode, emptyNode}; std::ranges::sort(ids);
	size_t requested = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t bytes) { requested = bytes; throw std::length_error("label entity budget"); };
	EXPECT_THROW(static_cast<void>(adapter.readNodeLabelMatchesSorted(snapshot, ids,
		std::array<uint64_t, 1U>{3U}, &session, &reject)), std::length_error);
	EXPECT_EQ(requested, ids.size() * sizeof(uint64_t));
	bool pageRejected = false;
	const SelectedPropertyReadAdmission rejectPage = [&](size_t bytes) {
		if (bytes >= RSS_PAGE_SIZE) { pageRejected = true; throw std::length_error("graph label page budget"); }
	};
	EXPECT_THROW(static_cast<void>(adapter.readNodeLabelMatchesSorted(snapshot, ids,
		std::array<uint64_t, 1U>{3U}, &session, &rejectPage)), std::length_error);
	EXPECT_TRUE(pageRejected);
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 1U, peak = 0U;
		const SelectedPropertyReadAdmission admit = [&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); };
		const auto admitted = adapter.readNodeLabelMatchesSorted(snapshot, ids, std::array<uint64_t, 1U>{3U}, &session, &admit);
		ASSERT_EQ(admitted.size(), ids.size());
		EXPECT_EQ(retained, admitted.capacity() / 8U + (admitted.capacity() % 8U != 0U ? 1U : 0U));
		EXPECT_GE(peak, ids.size() * (sizeof(NodeEntityRecord) + sizeof(uint8_t)));
	}
	for (int retry = 0; retry < 2; ++retry) {
		for (const auto &required: {std::vector<uint64_t>{}, std::vector<uint64_t>{3U, 7U}, std::vector<uint64_t>{17U}}) {
			const auto matches = adapter.readNodeLabelMatchesSorted(snapshot, ids, required, &session);
			ASSERT_EQ(matches.size(), ids.size());
			std::vector<std::pair<uint64_t, bool>> scanned;
			EXPECT_TRUE(adapter.visitNodeRefsWithLabelTokenMatchesUntil(snapshot, 8U, required,
				[&](const EntityRef &ref, bool match) { scanned.emplace_back(ref.entity_id, match); return true; }, &session));
			ASSERT_EQ(scanned.size(), ids.size());
			for (const auto &[id, match]: scanned) {
				const auto ordinal = std::ranges::lower_bound(ids, id);
				ASSERT_NE(ordinal, ids.end());
				EXPECT_EQ(match, matches[static_cast<size_t>(ordinal - ids.begin())]);
			}
			for (size_t index = 0U; index < ids.size(); ++index) {
				const bool expected = required.empty() ||
					(required.back() == 17U ? ids[index] == overflowNode : ids[index] != emptyNode);
				EXPECT_EQ(matches[index], expected);
			}
		}
	}
}

TEST_F(PageBackedGraphReadAdapterTest, LabelScanRejectsMissingOverflowBeforeDeliveringInlineMatch) {
	Superblock superblock; superblock.database_id = DatabaseId(102U);
	superblock.generation = Generation(1U); superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	NodeEntityRecord record;
	record.inline_label_ids = {3U, 7U, 11U};
	record.inline_label_count = 3U;
	record.label_overflow = {PageId(999U), 0U, 1U};
	const EntityPageNewPoolRequest pool{.segment_id = SegmentId(31U), .pool_owner_id = 901U,
		.generation = Generation(1U), .page_id = PageId(101U),
		.first_slot = PhysicalSlotId(40U), .last_slot = PhysicalSlotId(40U)};
	ASSERT_TRUE(entities.allocateNode(pool, 1001U, record, LogSequenceNumber(2U)).has_value());
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata); BTreePageStore trees(slotted, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot(); NativeReadSession session;
	for (int retry = 0; retry < 2; ++retry) {
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(adapter.visitNodeRefsWithLabelTokenMatchesUntil(snapshot, 8U,
			std::array<uint64_t, 1U>{3U}, [&](const EntityRef &, bool) { ++delivered; return true; },
			&session)), FormatException);
		EXPECT_EQ(delivered, 0U);
		EXPECT_FALSE(session.nodeLabels(1001U).has_value());
		EXPECT_THROW(static_cast<void>(adapter.nodeHasLabelTokens(snapshot, 1001U, std::array<uint64_t, 1U>{3U}, &session)), FormatException);
		EXPECT_THROW(static_cast<void>(adapter.readNodeLabelMatchesSorted(snapshot, std::array<uint64_t, 1U>{1001U},
			std::array<uint64_t, 1U>{3U}, &session)), FormatException);
	}
}

TEST_F(PageBackedGraphReadAdapterTest, CachedLabelViewBorrowsOwnedStorageAndRefreshesAfterReplacement) {
	NativeReadSession session;
	EXPECT_FALSE(session.nodeLabelsView(17U).has_value());
	std::vector<uint64_t> input{3U, 7U, 11U};
	session.putNodeLabels(17U, input);
	const auto first = session.nodeLabelsView(17U);
	ASSERT_TRUE(first.has_value());
	EXPECT_NE(first->data(), input.data());
	EXPECT_EQ(first->data(), session.nodeLabelsView(17U)->data());
	input[0] = 1U;
	EXPECT_EQ((*first)[0], 3U);
	// Do not reuse the borrowed view after replacing session storage.
	session.putNodeLabels(17U, {5U, 9U});
	const auto updated = session.nodeLabelsView(17U);
	ASSERT_TRUE(updated.has_value());
	EXPECT_EQ((std::vector<uint64_t>(updated->begin(), updated->end())), (std::vector<uint64_t>{5U, 9U}));
}

TEST_F(PageBackedGraphReadAdapterTest, LabelSessionEvictsByPayloadBytesAndReleasesReplacementCapacity) {
	NativeReadSession session;
	std::vector<uint64_t> labels(256U);
	for (size_t index = 0U; index < labels.size(); ++index) labels[index] = index + 1U;
	for (uint64_t id = 1U; id <= 100U; ++id) session.putNodeLabels(id, labels);
	EXPECT_FALSE(session.nodeLabelsView(1U).has_value());
	ASSERT_TRUE(session.nodeLabelsView(100U).has_value());
	size_t total = 0U;
	for (uint64_t id = 1U; id <= 100U; ++id) {
		if (const auto view = session.nodeLabelsView(id)) total += view->size_bytes();
	}
	EXPECT_LE(total, NativeReadSession::MAXIMUM_CACHED_NODE_LABEL_BYTES);
	session.putNodeLabels(100U, {7U});
	ASSERT_TRUE(session.nodeLabelsView(100U).has_value());
	EXPECT_EQ(session.nodeLabelsView(100U)->size(), 1U);
	session.putNodeLabels(101U, labels);
	EXPECT_TRUE(session.nodeLabelsView(101U).has_value());
}

TEST_F(PageBackedGraphReadAdapterTest, LabelCacheUsesByteLimitInsteadOfEntryCountForIndividualSets) {
	NativeReadSession session;
	std::vector<uint64_t> labels(NativeReadSession::MAXIMUM_CACHED_NODE_LABEL_SETS + 1U);
	for (size_t index = 0U; index < labels.size(); ++index) labels[index] = index + 1U;
	session.putNodeLabels(17U, labels);
	ASSERT_TRUE(session.nodeLabelsView(17U).has_value());
	EXPECT_EQ(session.nodeLabelsView(17U)->size(), labels.size());
	std::vector<uint64_t> oversized(NativeReadSession::MAXIMUM_CACHED_NODE_LABEL_BYTES / sizeof(uint64_t) + 1U);
	for (size_t index = 0U; index < oversized.size(); ++index) oversized[index] = index + 1U;
	session.putNodeLabels(18U, oversized);
	EXPECT_FALSE(session.nodeLabelsView(18U).has_value());
	EXPECT_TRUE(session.nodeLabelsView(17U).has_value());
	session.putNodeLabels(17U, oversized);
	EXPECT_FALSE(session.nodeLabelsView(17U).has_value());
	EXPECT_FALSE(session.nodeLabels(17U).has_value());
	for (int retry = 0; retry < 2; ++retry) {
		session.putNodeLabels(17U, labels);
		ASSERT_TRUE(session.nodeLabelsView(17U).has_value());
		EXPECT_EQ(session.nodeLabelsView(17U)->size(), labels.size());
	}
}

TEST_F(PageBackedGraphReadAdapterTest, PointPropertyCountReusesCursorPagesAndPreservesSnapshotCounts) {
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1U);
	superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata);
	BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U);
	options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U;
	options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(transaction.commit().committed());
	const PropertyCollectionRoot root{source, graph::Node::typeId,
		{{3U, graph::PropertyValue(11)}, {5U, graph::PropertyValue()}}};
	ASSERT_EQ(test::publishPropertyRoot(entities, properties, *metadata, root, options.property_roots).properties.size(), 1U);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	auto snapshot = metadata->openSnapshot();
	NativeReadSession session;
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	for (size_t retry = 0U; retry < 2U; ++retry) {
		graph::debug::PerfTrace::reset();
		EXPECT_EQ(adapter.nodePropertyCount(snapshot, source, &session), 2U);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
		if (retry == 0U) {
			ASSERT_TRUE(trace.contains("native_graph.read.property_count_batch_pages"));
			EXPECT_EQ(trace.at("native_graph.read.property_count_batch_pages").totalValue, 1);
		} else EXPECT_FALSE(trace.contains("native_graph.read.property_count_batch_pages"));
	}
	EXPECT_EQ(adapter.nodePropertyCount(snapshot, target, &session), 0U);
	const auto columns = adapter.readNodePropertyColumnsSorted(snapshot, std::array<uint64_t, 1U>{source},
		std::array<uint32_t, 2U>{3U, 5U}, &session);
	ASSERT_EQ(columns.size(), 2U); EXPECT_EQ(columns[0][0], graph::PropertyValue(11));
	EXPECT_EQ(columns[1][0], graph::PropertyValue());
	const PropertyCollectionRoot replacement{source, graph::Node::typeId, {{3U, graph::PropertyValue(12)}}};
	ASSERT_EQ(test::publishPropertyRoot(entities, properties, *metadata, replacement, options.property_roots).properties.size(), 1U);
	EXPECT_EQ(adapter.nodePropertyCount(snapshot, source, &session), 2U);
	auto current = metadata->openSnapshot(); NativeReadSession currentSession;
	EXPECT_EQ(adapter.nodePropertyCount(current, source, &currentSession), 1U);
	EXPECT_THROW(static_cast<void>(adapter.nodePropertyCount(snapshot, 0U, &session)), std::exception);
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	current.release(); snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, SelectedPointReadsPreserveCachedPayloadAndDoNotRepublishHits) {
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1U);
	superblock.checkpoint_lsn = LogSequenceNumber(1U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata);
	BTreePageStore trees(slotted, *metadata);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(46U);
	options.last_slot = PhysicalSlotId(70U);
	options.entity_pool_owner_id = 907U;
	options.property_roots = {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U};
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, options);
	auto transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = transaction.createEdge(source, target, 41U);
	ASSERT_TRUE(transaction.commit().committed());
	class Names final : public PageBackedGraphNameResolver {
	public:
		std::string resolveNodeLabelToken(const MetadataSnapshotPin &, uint64_t) const override { return "Node"; }
		std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &, uint64_t) const override { return "LINK"; }
		std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t token) const override { return "key" + std::to_string(token); }
	} names;
	const graph::PropertyValue payload(std::string(512U, 'p'));
	const std::array roots{PropertyCollectionRoot{source, graph::Node::typeId, {{3U, payload}, {5U, graph::PropertyValue()}}},
		PropertyCollectionRoot{edgeId, graph::Edge::typeId, {{3U, payload}, {5U, graph::PropertyValue()}}}};
	ASSERT_EQ(test::publishPropertyRoots(entities, properties, *metadata, roots, options.property_roots).properties.size(), 2U);
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	auto snapshot = metadata->openSnapshot();
	for (bool isEdge: {false, true}) {
		SCOPED_TRACE(isEdge);
		NativeReadSession session;
		const uint64_t id = isEdge ? edgeId : source;
		const std::array<uint64_t, 1U> ids{id}; const std::array<uint32_t, 3U> tokens{3U, 5U, 7U};
		static_cast<void>(isEdge ? pages.readEdgePropertyColumnsSorted(snapshot, ids, tokens, &session)
			: pages.readNodePropertyColumnsSorted(snapshot, ids, tokens, &session));
		for (size_t index = 0U; index < NativeReadSession::MAXIMUM_CACHED_PROPERTIES - tokens.size(); ++index)
			session.putProperty(isEdge, 1000U + index, 3U, graph::PropertyValue(1));
		const EntityReadShape shape{.property_tokens = {3U, 5U, 7U}};
		auto selected = isEdge ? logical.readEdgeShaped(snapshot, id, shape, &session).properties
			: logical.readNodeShaped(snapshot, id, shape, &session).properties;
		ASSERT_EQ(selected.size(), 2U); EXPECT_EQ(selected[0].value, payload); EXPECT_EQ(selected[1].value, graph::PropertyValue());
		const auto batch = isEdge ? logical.readEdgesShapedSorted(snapshot, ids, shape, &session).front().properties
			: logical.readNodesShapedSorted(snapshot, ids, shape, &session).front().properties;
		EXPECT_EQ(batch, selected);
		const auto reference = isEdge ? logical.readEdgeRef(snapshot, id, shape, &session)
			: logical.readNodeRef(snapshot, id, shape, &session);
		EXPECT_EQ(reference.entity_id, id); EXPECT_EQ(reference.is_edge, isEdge);
		selected[0].value = graph::PropertyValue("changed output");
		const auto cached = session.property(isEdge, id, 3U);
		ASSERT_TRUE(cached && *cached); EXPECT_EQ(**cached, payload);
		// A cache hit must not publish the same value again and refresh FIFO.
		session.putProperty(isEdge, 100000U, 3U, graph::PropertyValue(2));
		EXPECT_FALSE(session.hasProperty(isEdge, id, 3U));
		EXPECT_TRUE(session.hasProperty(isEdge, id, 5U));
		EXPECT_TRUE(session.hasProperty(isEdge, 1000U, 3U));
	}
	snapshot.release();
}
