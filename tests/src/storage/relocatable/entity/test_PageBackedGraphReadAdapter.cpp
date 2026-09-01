/**
 * @file test_PageBackedGraphReadAdapter.cpp
 * @brief Tests for the logical read view of native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest nodePool(uint64_t segmentId, uint64_t ownerId, uint64_t pageId, uint64_t physicalSlot) {
		return {.segment_id = SegmentId(segmentId),
				.pool_owner_id = ownerId,
				.generation = Generation(1U),
				.page_id = PageId(pageId),
				.first_slot = PhysicalSlotId(physicalSlot),
				.last_slot = PhysicalSlotId(physicalSlot)};
	}

	NodeEntityRecord node(std::array<uint64_t, RSS_ENTITY_INLINE_LABEL_CAPACITY> labels, uint8_t count) {
		return {.inline_label_ids = labels,
				.inline_label_count = count};
	}

	std::vector<uint64_t> labelIds(uint64_t first, size_t count) {
		std::vector<uint64_t> result;
		result.reserve(count);
		for (size_t index = 0U; index < count; ++index) {
			result.push_back(first + static_cast<uint64_t>(index) * 2U);
		}
		return result;
	}

	PropertyCollectionRoot propertyRoot(uint64_t ownerId, uint32_t ownerType,
										std::vector<PropertyCollectionRootEntry> entries) {
		return {ownerId, ownerType, std::move(entries)};
	}

	PropertyCollectionRootStoreOptions propertyOptions() { return {PhysicalSlotId(50U), PhysicalSlotId(70U), 903U}; }

	NativeGraphWritePlannerOptions graphWriteOptions() {
		NativeGraphWritePlannerOptions result;
		result.first_slot = PhysicalSlotId(46U);
		result.last_slot = PhysicalSlotId(70U);
		result.entity_pool_owner_id = 907U;
		result.property_roots = propertyOptions();
		return result;
	}

	class TestPageBackedGraphNameResolver final : public PageBackedGraphNameResolver {
	public:
		explicit TestPageBackedGraphNameResolver(bool duplicateLabels = false) : duplicate_labels_(duplicateLabels) {}

		[[nodiscard]] std::string resolveNodeLabelToken(const MetadataSnapshotPin &, uint64_t tokenId) const override {
			++node_label_resolutions_;
			switch (tokenId) {
				case 7U:
					return "Person";
				case 9U:
					return duplicate_labels_ ? "Person" : "VIP";
				default:
					return {};
			}
		}

		[[nodiscard]] std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &,
																														 uint64_t tokenId) const override {
			++relationship_type_resolutions_;
			switch (tokenId) {
				case 41U:
					return "KNOWS";
				case 43U:
					return "FOLLOWS";
				default:
					return {};
			}
		}

		[[nodiscard]] std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t tokenId) const override {
			++property_key_resolutions_;
			switch (tokenId) {
				case 3U:
					return "name";
				case 5U:
					return "age";
				case 7U:
					return "weight";
				case 9U:
					return "name";
				default:
					return {};
			}
		}

		[[nodiscard]] uint64_t relationshipTypeResolutionCount() const { return relationship_type_resolutions_; }
		[[nodiscard]] uint64_t propertyKeyResolutionCount() const { return property_key_resolutions_; }

	private:
		bool duplicate_labels_ = false;
		mutable uint64_t node_label_resolutions_ = 0U;
		mutable uint64_t relationship_type_resolutions_ = 0U;
		mutable uint64_t property_key_resolutions_ = 0U;
	};

} // namespace

class PageBackedGraphReadAdapterTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("page_backed_graph_read_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};

TEST_F(PageBackedGraphReadAdapterTest, ResolvesLogicalIdentityAndCompleteOverflowLabelsThroughOneSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(31U, 901U, 101U, 40U), 1001U, node({7U, 9U, 11U}, 3U),
									  LogSequenceNumber(2U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);

	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(before, 1001U), (PageBackedGraphNode{1001U, {7U, 9U, 11U}, {}}));

	const std::vector<uint64_t> overflowLabels = labelIds(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 3U);
	EntityLabelSetWritePlan plan;
	plan.append_page = EntityLabelSetAppendRequest{SegmentId(31U), PageId(102U)};
	static_cast<void>(labels.updateNodeLabels(before, 1001U, overflowLabels, plan, LogSequenceNumber(3U)));

	// The original Entity Map and Node-page frame remain reachable to this pin.
	EXPECT_EQ(adapter.readNode(before, 1001U), (PageBackedGraphNode{1001U, {7U, 9U, 11U}, {}}));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(current, 1001U), (PageBackedGraphNode{1001U, overflowLabels, {}}));
	before.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReturnsFullEdgeTopologyWithoutPersistedLogicalIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	const EdgeEntityRecord edge{1001U, 1002U, 41U};
	ASSERT_TRUE(entities.allocateEdge(nodePool(32U, 902U, 201U, 41U), 2001U, edge, LogSequenceNumber(2U)).has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(adapter.readEdge(snapshot, 2001U), (PageBackedGraphEdge{2001U, 1001U, 1002U, 41U, {}}));
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReadsOwnerKeyedPropertiesAndPreservesAnOlderSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(
			entities.allocateNode(nodePool(33U, 903U, 301U, 42U), 1001U, node({7U, 0U, 0U}, 1U), LogSequenceNumber(2U))
					.has_value());
	const EdgeEntityRecord edge{1001U, 1002U, 41U};
	ASSERT_TRUE(entities.allocateEdge(nodePool(34U, 904U, 302U, 43U), 2001U, edge, LogSequenceNumber(3U)).has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);

	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_TRUE(adapter.readNode(before, 1001U).properties.empty());
	EXPECT_TRUE(adapter.readEdge(before, 2001U).properties.empty());
	EXPECT_EQ(logical.readEdge(before, 2001U).type, "KNOWS");
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 1U);

	const std::vector<PropertyCollectionRootEntry> nodeProperties{{3U, graph::PropertyValue("alpha")},
																  {5U, graph::PropertyValue(int64_t(42))}};
	const std::vector<PropertyCollectionRootEntry> edgeProperties{{7U, graph::PropertyValue(true)}};
	const std::array roots{propertyRoot(1001U, graph::Node::typeId, nodeProperties),
						   propertyRoot(2001U, graph::Edge::typeId, edgeProperties)};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(current, 1001U).properties, nodeProperties);
	EXPECT_EQ(adapter.readEdge(current, 2001U).properties, edgeProperties);
	EXPECT_EQ(logical.readEdge(current, 2001U).type, "KNOWS");
	// A new metadata generation cannot reuse a logical-name cache entry from
	// the earlier pinned snapshot, even when the durable token name is unchanged.
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	EXPECT_TRUE(adapter.readNode(before, 1001U).properties.empty());
	EXPECT_TRUE(adapter.readEdge(before, 2001U).properties.empty());
	before.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ConvertsNativeRecordsToDirectOwnerKeyedLogicalProperties) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(42U, 912U, 801U, 51U), 1001U, node({7U, 9U, 0U}, 2U),
																								 LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(43U, 913U, 802U, 52U), 2001U,
											 EdgeEntityRecord{1001U, 1002U, 41U},
														 LogSequenceNumber(3U))
					.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	const std::array roots{
			propertyRoot(1001U, graph::Node::typeId,
						 {{3U, graph::PropertyValue("Ada")}, {5U, graph::PropertyValue(int64_t(42))}}),
			propertyRoot(2001U, graph::Edge::typeId, {{7U, graph::PropertyValue(int64_t(2))}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter adapter(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_EQ(adapter.readNode(snapshot, 1001U),
			  (LogicalGraphNode{1001U,
									{"Person", "VIP"},
									{{"name", graph::PropertyValue("Ada")}, {"age", graph::PropertyValue(int64_t(42))}}}));
	EXPECT_EQ(adapter.readEdge(snapshot, 2001U),
			  (LogicalGraphEdge{2001U,
									1001U,
									1002U,
									"KNOWS",
									{{"weight", graph::PropertyValue(int64_t(2))}}}));
	const LogicalGraphNodePage page = adapter.scanNodesAfter(snapshot, 0U, 1U, 8U);
	ASSERT_EQ(page.nodes.size(), 1U);
	EXPECT_EQ(page.nodes.front().properties.size(), 2U);
	TestPageBackedGraphNameResolver duplicateNames(true);
	PageBackedLogicalGraphReadAdapter duplicateAdapter(pages, duplicateNames);
	EXPECT_THROW(static_cast<void>(duplicateAdapter.readNode(snapshot, 1001U)), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsUnknownLogicalNamesButAllowsAnUntypedNativeRelationship) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(44U, 914U, 803U, 53U), 1001U, node({11U, 0U, 0U}, 1U),
																										 LogSequenceNumber(2U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(45U, 915U, 804U, 54U), 2001U,
																								 EdgeEntityRecord{1001U, 1002U, 99U},
																								 LogSequenceNumber(3U))
					.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(46U, 916U, 805U, 55U), 2003U,
																								 EdgeEntityRecord{1001U, 1002U, 0U},
																								 LogSequenceNumber(4U))
					.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2001U)), FormatException);
	const LogicalGraphEdge untyped = logical.readEdge(snapshot, 2003U);
	EXPECT_TRUE(untyped.type.empty());
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsDuplicateAndUnknownPropertyNamesAtTheLogicalBoundary) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(48U, 918U, 807U, 57U), 1001U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(entities.allocateNode(nodePool(49U, 919U, 808U, 58U), 1003U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(3U))
						.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(50U, 920U, 809U, 59U), 2001U,
																							 EdgeEntityRecord{1001U, 1003U, 41U},
																							 LogSequenceNumber(4U))
						.has_value());
	ASSERT_TRUE(entities.allocateEdge(nodePool(51U, 921U, 810U, 60U), 2003U,
																							 EdgeEntityRecord{1003U, 1001U, 41U},
																							 LogSequenceNumber(5U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	const std::array roots{
			propertyRoot(1001U, graph::Node::typeId,
						 {{3U, graph::PropertyValue("Ada")}, {9U, graph::PropertyValue("duplicate")}}),
			propertyRoot(1003U, graph::Node::typeId, {{11U, graph::PropertyValue("unknown")}}),
			propertyRoot(2001U, graph::Edge::typeId,
						 {{3U, graph::PropertyValue("first")}, {9U, graph::PropertyValue("duplicate")}}),
			propertyRoot(2003U, graph::Edge::typeId, {{11U, graph::PropertyValue("unknown")}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 1003U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2001U)), FormatException);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 2003U)), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ScansNativeEntityMapInLogicalOrderWithResumableNodeBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(35U, 905U, 401U, 44U), 1001U, node({7U, 0U, 0U}, 1U),
																								 LogSequenceNumber(2U))
					.has_value());
	MetadataSnapshotPin afterFirstNode = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(afterFirstNode, PageId(401U), 1003U, node({9U, 0U, 0U}, 1U),
																									 LogSequenceNumber(3U)));
	MetadataSnapshotPin afterSecondNode = metadata->openSnapshot();
	static_cast<void>(entities.insertNode(afterSecondNode, PageId(401U), 1005U, node({11U, 0U, 0U}, 1U),
																									  LogSequenceNumber(4U)));
	ASSERT_TRUE(entities.allocateEdge(nodePool(36U, 906U, 402U, 45U), 2001U,
											 EdgeEntityRecord{1001U, 1003U, 31U},
														 LogSequenceNumber(5U))
					.has_value());
	MetadataSnapshotPin afterFirstEdge = metadata->openSnapshot();
	static_cast<void>(entities.insertEdge(afterFirstEdge, PageId(402U), 2003U,
																	  EdgeEntityRecord{1003U, 1005U, 33U},
																			  LogSequenceNumber(6U)));
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);

	std::vector<uint64_t> oldNodeIds;
	EXPECT_TRUE(adapter.visitNodesUntil(afterFirstNode, 4U, [&oldNodeIds](const PageBackedGraphNode &record) {
		oldNodeIds.push_back(record.node_id);
		return true;
	}));
	EXPECT_EQ(oldNodeIds, (std::vector<uint64_t>{1001U}));

	MetadataSnapshotPin current = metadata->openSnapshot();
	std::vector<uint64_t> boundedNodeIds;
	EXPECT_FALSE(adapter.visitNodesUntil(current, 2U, [&boundedNodeIds](const PageBackedGraphNode &record) {
		boundedNodeIds.push_back(record.node_id);
		return true;
	}));
	EXPECT_EQ(boundedNodeIds, (std::vector<uint64_t>{1001U, 1003U}));
	const PageBackedGraphNodePage firstPage = adapter.scanNodesAfter(current, 1001U, 1U, 8U);
	ASSERT_EQ(firstPage.nodes.size(), 1U);
	EXPECT_EQ(firstPage.nodes.front().node_id, 1003U);
	EXPECT_TRUE(firstPage.may_have_more);
	EXPECT_GT(firstPage.metadata_pages_visited, 0U);
	EXPECT_LE(firstPage.metadata_pages_visited, 8U);
	const PageBackedGraphNodePage finalPage = adapter.scanNodesAfter(current, 1003U, 4U, 8U);
	ASSERT_EQ(finalPage.nodes.size(), 1U);
	EXPECT_EQ(finalPage.nodes.front().node_id, 1005U);
	EXPECT_FALSE(finalPage.may_have_more);
	const PageBackedGraphNodePage terminalPage =
			adapter.scanNodesAfter(current, std::numeric_limits<uint64_t>::max(), 1U, 1U);
	EXPECT_TRUE(terminalPage.nodes.empty());
	EXPECT_FALSE(terminalPage.may_have_more);
	EXPECT_EQ(terminalPage.metadata_pages_visited, 0U);

	std::vector<uint64_t> edgeIds;
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	EXPECT_TRUE(adapter.visitEdgesUntil(current, 4U, [&edgeIds](const PageBackedGraphEdge &record) {
		edgeIds.push_back(record.edge_id);
		return true;
	}));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{2001U, 2003U}));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_native_materialization"));
	EXPECT_TRUE(trace.contains("native_graph.read.edge_logical_visitor"));
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(current, 0U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(current, 0U, 1U, 0U)), FormatException);
	afterFirstNode.release();
	afterSecondNode.release();
	afterFirstEdge.release();
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ResumesNativeAdjacencyThroughLogicalEdgeIdsOnly) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = transaction.createEdge(source, firstTarget, 41U);
	const uint64_t secondEdge = transaction.createEdge(source, secondTarget, 43U);
	ASSERT_TRUE(transaction.commit().committed());
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin current = metadata->openSnapshot();

	const PageBackedGraphAdjacencyPage firstPage = adapter.scanAdjacentEdges(
			current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(firstPage.edges.size(), 1U);
	EXPECT_EQ(firstPage.edges.front().edge_id, firstEdge);
	ASSERT_TRUE(firstPage.continuation.has_value());
	EXPECT_EQ(*firstPage.continuation, (PageBackedGraphAdjacencyContinuation{firstEdge, secondEdge}));
	const PageBackedGraphAdjacencyPage finalPage = adapter.scanAdjacentEdges(
			current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, firstPage.continuation, 1U);
	ASSERT_EQ(finalPage.edges.size(), 1U);
	EXPECT_EQ(finalPage.edges.front().edge_id, secondEdge);
	EXPECT_FALSE(finalPage.continuation.has_value());
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
				 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING,
				 PageBackedGraphAdjacencyContinuation{firstEdge, secondEdge + 1U}, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
				 current, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U,
				 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);

	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);
	std::vector<LogicalGraphEdgeTopology> topologies;
	EXPECT_TRUE(logical.visitEdgeTopologiesUntil(current, 3U, [&topologies](const LogicalGraphEdgeTopology &record) {
		topologies.push_back(record);
		return true;
	}));
	EXPECT_EQ(topologies,
			  (std::vector<LogicalGraphEdgeTopology>{{firstEdge, source, firstTarget, "KNOWS"},
														 {secondEdge, source, secondTarget, "FOLLOWS"}}));
	// Topology-only consumers must not enter the owner Property namespace.
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	std::vector<LogicalGraphEdgeTopology> adjacentTopologies;
	EXPECT_TRUE(logical.visitAdjacentEdgeTopologiesUntil(
			current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
			[&adjacentTopologies](const LogicalGraphEdgeTopology &record) {
				adjacentTopologies.push_back(record);
				return true;
			}));
	EXPECT_EQ(adjacentTopologies,
			  (std::vector<LogicalGraphEdgeTopology>{{firstEdge, source, firstTarget, "KNOWS"},
																 {secondEdge, source, secondTarget, "FOLLOWS"}}));
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	const LogicalGraphAdjacencyPage logicalFirst =
			logical.scanAdjacentEdges(current, source, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(logicalFirst.edges.size(), 1U);
	EXPECT_EQ(logicalFirst.edges.front().edge_id, firstEdge);
	EXPECT_EQ(logicalFirst.edges.front().type, "KNOWS");
	ASSERT_TRUE(logicalFirst.continuation.has_value());
	EXPECT_EQ(*logicalFirst.continuation, (LogicalGraphAdjacencyContinuation{firstEdge, secondEdge}));
	const LogicalGraphAdjacencyPage logicalFinal = logical.scanAdjacentEdges(
			current, source, LogicalGraphReadDirection::LGRD_OUTGOING, logicalFirst.continuation, 1U);
	ASSERT_EQ(logicalFinal.edges.size(), 1U);
	EXPECT_EQ(logicalFinal.edges.front().edge_id, secondEdge);
	EXPECT_EQ(logicalFinal.edges.front().type, "FOLLOWS");
	EXPECT_FALSE(logicalFinal.continuation.has_value());

	const LogicalGraphAdjacencyPage incoming =
			logical.scanAdjacentEdges(current, firstTarget, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U);
	ASSERT_EQ(incoming.edges.size(), 1U);
	EXPECT_EQ(incoming.edges.front().edge_id, firstEdge);
	EXPECT_THROW(static_cast<void>(logical.scanAdjacentEdges(
				 current, source, static_cast<LogicalGraphReadDirection>(99U), std::nullopt, 1U)),
			 FormatException);

	PageBackedLogicalGraphAdjacencyCursor cursor =
			logical.openAdjacentEdgeCursor(current, source, LogicalGraphReadDirection::LGRD_OUTGOING);
	const LogicalGraphAdjacencyPage cursorFirst = cursor.next(current, std::nullopt, 1U);
	ASSERT_TRUE(cursorFirst.continuation.has_value());
	EXPECT_EQ(cursorFirst.edges.front().edge_id, firstEdge);
	const LogicalGraphAdjacencyPage cursorFinal = cursor.next(current, cursorFirst.continuation, 1U);
	ASSERT_EQ(cursorFinal.edges.size(), 1U);
	EXPECT_EQ(cursorFinal.edges.front().edge_id, secondEdge);
	EXPECT_FALSE(cursorFinal.continuation.has_value());

	std::vector<uint64_t> logicalNodes;
	EXPECT_TRUE(logical.visitNodesUntil(current, 4U, [&logicalNodes](const LogicalGraphNode &record) {
		logicalNodes.push_back(record.node_id);
		return true;
	}));
	EXPECT_EQ(logicalNodes, (std::vector<uint64_t>{source, firstTarget, secondTarget}));
	std::vector<uint64_t> logicalEdges;
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	EXPECT_TRUE(logical.visitEdgesUntil(current, 3U, [&logicalEdges](const LogicalGraphEdge &record) {
		logicalEdges.push_back(record.edge_id);
		return true;
	}));
	graph::debug::PerfTrace::setEnabled(false);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_EQ(logicalEdges, (std::vector<uint64_t>{firstEdge, secondEdge}));
	EXPECT_GT(trace.at("native_graph.read.edge_entity_record").calls, 0U);
	EXPECT_GT(trace.at("native_graph.read.edge_name_materialization").calls, 0U);
	std::vector<uint64_t> visitedAdjacent;
	EXPECT_TRUE(logical.visitAdjacentEdgesUntil(
			current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
			[&visitedAdjacent](const LogicalGraphEdge &record) {
				visitedAdjacent.push_back(record.edge_id);
				return true;
			}));
	EXPECT_EQ(visitedAdjacent, (std::vector<uint64_t>{firstEdge, secondEdge}));
	// The same pinned snapshot resolves each relationship token once even when
	// multiple traversal APIs revisit the same native Edge records.
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);
	EXPECT_THROW(static_cast<void>(logical.visitNodesUntil(current, 3U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitEdgesUntil(current, 2U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(logical.visitAdjacentEdgesUntil(
				 current, source, LogicalGraphReadDirection::LGRD_OUTGOING, 2U, {})),
			 FormatException);
	current.release();
}

TEST_F(PageBackedGraphReadAdapterTest, StreamsSupernodeVisitorsInBoundedLogicalBatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	std::vector<uint64_t> expected;
	expected.reserve(257U);
	for (size_t ordinal = 0U; ordinal < 257U; ++ordinal) {
		const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
		expected.push_back(transaction.createEdge(source, target, 41U));
	}
	ASSERT_TRUE(transaction.commit().committed());

	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	std::vector<uint64_t> visited;
	EXPECT_TRUE(adapter.visitAdjacentEdgesUntil(
			snapshot, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, expected.size(),
			[&visited](const PageBackedGraphEdge &edge) {
				visited.push_back(edge.edge_id);
				return true;
			}));
	EXPECT_EQ(visited, expected);

	size_t earlyVisits = 0U;
	EXPECT_FALSE(adapter.visitAdjacentEdgesUntil(
			snapshot, source, PageBackedGraphReadDirection::PBGRD_OUTGOING, expected.size(),
			[&earlyVisits](const PageBackedGraphEdge &) {
				++earlyVisits;
				return false;
			}));
	EXPECT_EQ(earlyVisits, 1U);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, StreamsLargeAdjacencyTopologyThroughOneEntityMapPass) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, trees, graphWriteOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 0U>{});
	constexpr size_t EDGE_COUNT = 513U;
	std::vector<uint64_t> expectedEdgeIds;
	expectedEdgeIds.reserve(EDGE_COUNT);
	for (size_t ordinal = 0U; ordinal < EDGE_COUNT; ++ordinal) {
		const uint64_t target = transaction.createNode(std::array<uint64_t, 0U>{});
		expectedEdgeIds.push_back(transaction.createEdge(source, target, ordinal % 2U == 0U ? 41U : 43U));
	}
	ASSERT_TRUE(transaction.commit().committed());

	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	const std::array propertyRoots{
		propertyRoot(expectedEdgeIds.front(), graph::Edge::typeId, {{7U, graph::PropertyValue("present")}})};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, propertyRoots, propertyOptions()));
	TestPageBackedGraphNameResolver names;
	PageBackedLogicalGraphReadAdapter logical(adapter, names);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	size_t visited = 0U;
	uint64_t firstTarget = 0U;
	uint64_t lastEdgeId = 0U;
	EXPECT_TRUE(logical.visitAdjacentEdgeTopologiesUntil(snapshot, source, LogicalGraphReadDirection::LGRD_OUTGOING,
														 EDGE_COUNT + 1U, [&](const LogicalGraphEdgeTopology &edge) {
															 if (visited == 0U) {
																 firstTarget = edge.target_node_id;
															 }
															 ++visited;
															 lastEdgeId = edge.edge_id;
															 return true;
														 }));
	EXPECT_EQ(visited, EDGE_COUNT);
	EXPECT_EQ(lastEdgeId, expectedEdgeIds.back());
	EXPECT_NE(firstTarget, 0U);
	EXPECT_EQ(names.propertyKeyResolutionCount(), 0U);
	EXPECT_EQ(names.relationshipTypeResolutionCount(), 2U);

	visited = 0U;
	EXPECT_FALSE(logical.visitAdjacentEdgeTopologiesUntil(snapshot, source, LogicalGraphReadDirection::LGRD_OUTGOING,
														  EDGE_COUNT + 1U, [&](const LogicalGraphEdgeTopology &) {
															  ++visited;
															  return visited < 3U;
														  }));
	EXPECT_EQ(visited, 3U);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, ReopensAndScansNativeEntityMapWithoutFixedRecords) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		EntityPageStore entities(io, *metadata);
		ASSERT_TRUE(entities.allocateNode(nodePool(39U, 909U, 601U, 48U), 1001U, node({7U, 0U, 0U}, 1U),
																								   LogSequenceNumber(2U))
					.has_value());
		MetadataSnapshotPin first = metadata->openSnapshot();
		static_cast<void>(entities.insertNode(first, PageId(601U), 1002U, node({9U, 0U, 0U}, 1U),
																										LogSequenceNumber(3U)));
		first.release();
	}

	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *reopened);
	SlottedPagePool slottedPages(io, *reopened);
	EntityLabelSetStore labels(io, *reopened, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *reopened);
	BTreePageStore trees(slottedPages, *reopened);
	PageBackedGraphReadAdapter adapter(*reopened, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const PageBackedGraphNodePage page = adapter.scanNodesAfter(snapshot, 0U, 4U, 8U);
	ASSERT_EQ(page.nodes.size(), 2U);
	EXPECT_EQ(page.nodes[0].node_id, 1001U);
	EXPECT_EQ(page.nodes[1].node_id, 1002U);
	EXPECT_FALSE(page.may_have_more);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsACorruptEntityMapThatPointsANodeToAnEdgePage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(40U, 910U, 701U, 49U), 1001U, node({7U, 0U, 0U}, 1U),
																								 LogSequenceNumber(2U))
					.has_value());
	const std::optional<EntityPageInsertResult> edge = entities.allocateEdge(
			nodePool(41U, 911U, 702U, 50U), 2001U, EdgeEntityRecord{1001U, 1002U, 41U},
			LogSequenceNumber(3U));
	ASSERT_TRUE(edge.has_value());
	const EntityPageLocation corruptNodeLocation{1002U, edge->location.page_id, edge->location.slot_id,
																										 edge->location.slot_generation};
	const std::array corruptNodes{corruptNodeLocation};
	static_cast<void>(metadata->upsertEntityPageLocations(corruptNodes, {}, {}, {}, LogSequenceNumber(4U)));
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(snapshot, 4U, [](const PageBackedGraphNode &) {
		return true;
	})), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsZeroLogicalIdsBeforeAccessingDirectories) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(adapter.readNode(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.readEdge(snapshot, 0U)), FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsInvalidAdjacencyBoundsDirectionsAndContinuationBeforeScanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	ASSERT_TRUE(entities.allocateNode(nodePool(47U, 917U, 806U, 56U), 1001U, node({7U, 0U, 0U}, 1U),
																							 LogSequenceNumber(2U))
						.has_value());
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);

	EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, properties, trees, 0U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(
					 *metadata, entities, labels, properties, trees, std::numeric_limits<size_t>::max())),
			 std::invalid_argument);
	PageBackedGraphReadAdapter adapter(*metadata, entities, labels, properties, trees, 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 0U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt, 0U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, std::nullopt,
			 std::numeric_limits<size_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING,
			 PageBackedGraphAdjacencyContinuation{0U, 1U}, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING,
			 PageBackedGraphAdjacencyContinuation{1U, 0U}, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanAdjacentEdges(
			 snapshot, 1001U, static_cast<PageBackedGraphReadDirection>(99U), std::nullopt, 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.openAdjacentEdgeCursor(
			 snapshot, 0U, PageBackedGraphReadDirection::PBGRD_OUTGOING)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.openAdjacentEdgeCursor(
			 snapshot, 1001U, static_cast<PageBackedGraphReadDirection>(99U))),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.scanNodesAfter(snapshot, 0U, 0U, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitNodesUntil(snapshot, 1U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgesUntil(snapshot, 0U,
			 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitEdgesUntil(snapshot, 1U, {})), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 0U,
			 [](const PageBackedGraphEdge &) { return true; })),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.visitAdjacentEdgesUntil(
			 snapshot, 1001U, PageBackedGraphReadDirection::PBGRD_OUTGOING, 1U, {})),
			 FormatException);
	snapshot.release();
}

TEST_F(PageBackedGraphReadAdapterTest, RejectsStoresFromAnotherMetadataDomain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	BTreePageStore trees(slottedPages, *metadata);

	const std::filesystem::path foreignPath = path.string() + "_foreign";
	{
		auto foreignFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																			   std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		EntityPageStore foreignEntities(foreignIo, *foreignMetadata);
		SlottedPagePool foreignSlottedPages(foreignIo, *foreignMetadata);
		EntityLabelSetStore foreignLabels(foreignIo, *foreignMetadata, foreignSlottedPages);
		PropertyCollectionRootStore foreignProperties(foreignSlottedPages, *foreignMetadata);
		BTreePageStore foreignTrees(foreignSlottedPages, *foreignMetadata);

		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, foreignEntities, labels, properties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, foreignLabels, properties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, foreignProperties, trees)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(PageBackedGraphReadAdapter(*metadata, entities, labels, properties, foreignTrees)),
					 std::invalid_argument);
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}
