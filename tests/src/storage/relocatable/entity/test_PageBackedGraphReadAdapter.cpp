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
			return tokenId == 41U ? "KNOWS" : std::string{};
		}

		[[nodiscard]] std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t tokenId) const override {
			switch (tokenId) {
				case 3U:
					return "name";
				case 5U:
					return "age";
				case 7U:
					return "weight";
				default:
					return {};
			}
		}

	private:
		bool duplicate_labels_ = false;
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

	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_TRUE(adapter.readNode(before, 1001U).properties.empty());
	EXPECT_TRUE(adapter.readEdge(before, 2001U).properties.empty());

	const std::vector<PropertyCollectionRootEntry> nodeProperties{{3U, graph::PropertyValue("alpha")},
																  {5U, graph::PropertyValue(int64_t(42))}};
	const std::vector<PropertyCollectionRootEntry> edgeProperties{{7U, graph::PropertyValue(true)}};
	const std::array roots{propertyRoot(1001U, graph::Node::typeId, nodeProperties),
						   propertyRoot(2001U, graph::Edge::typeId, edgeProperties)};
	static_cast<void>(test::publishPropertyRoots(entities, properties, *metadata, roots, propertyOptions()));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(adapter.readNode(current, 1001U).properties, nodeProperties);
	EXPECT_EQ(adapter.readEdge(current, 2001U).properties, edgeProperties);
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
	EXPECT_TRUE(adapter.visitEdgesUntil(current, 4U, [&edgeIds](const PageBackedGraphEdge &record) {
		edgeIds.push_back(record.edge_id);
		return true;
	}));
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{2001U, 2003U}));
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
