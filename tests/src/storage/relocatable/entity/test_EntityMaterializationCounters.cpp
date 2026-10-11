#include <array>
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace {
class EmptyCounterNames final : public PageBackedGraphNameResolver {
public:
	std::string resolveNodeLabelToken(const MetadataSnapshotPin &, uint64_t) const override { return {}; }
	std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &, uint64_t) const override { return {}; }
	std::string resolvePropertyKey(const MetadataSnapshotPin &, uint32_t) const override { return {}; }
};
}

TEST_F(RelocatableMetadataStoreTest, EntityMaterializationCountersIncludePropertylessPointBatchAndScan) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *metadata);
	EntityPageNewPoolRequest pool{.segment_id = SegmentId(31U), .pool_owner_id = 901U,
		.generation = Generation(1U), .page_id = PageId(101U),
		.first_slot = PhysicalSlotId(40U), .last_slot = PhysicalSlotId(40U)};
	ASSERT_TRUE(entities.allocateNode(pool, 1001U, NodeEntityRecord{}, LogSequenceNumber(2U)).has_value());
	pool.segment_id = SegmentId(32U); pool.pool_owner_id = 902U; pool.page_id = PageId(201U);
	pool.first_slot = PhysicalSlotId(41U); pool.last_slot = PhysicalSlotId(41U);
	ASSERT_TRUE(entities.allocateEdge(pool, 2001U, EdgeEntityRecord{1001U, 1002U, 0U}, LogSequenceNumber(3U)).has_value());
	SlottedPagePool slotted(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, slotted);
	PropertyCollectionRootStore properties(slotted, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PageBackedGraphReadAdapter pages(*metadata, entities, labels, properties, trees);
	EmptyCounterNames names;
	PageBackedLogicalGraphReadAdapter logical(pages, names);
	auto snapshot = metadata->openSnapshot();
	using graph::debug::PerfTrace;
	struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::reset(); } } cleanup;
	PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(true); PerfTrace::reset();
	EXPECT_TRUE(logical.readNode(snapshot, 1001U).properties.empty());
	EXPECT_TRUE(logical.readEdge(snapshot, 2001U).properties.empty());
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("native_graph.read.full_materializations").totalValue, 2);
	const std::array<uint64_t, 1U> nodes{1001U}, edges{2001U};
	EntityReadShape complete; complete.all_properties = true;
	EXPECT_EQ(logical.readNodesShapedSorted(snapshot, nodes, complete).size(), 1U);
	EXPECT_EQ(logical.readEdgesShapedSorted(snapshot, edges, complete).size(), 1U);
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("native_graph.read.full_materializations").totalValue, 2);
	EXPECT_EQ(logical.scanNodesAfter(snapshot, 0U, 10U, 100U).nodes.size(), 1U);
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("native_graph.read.full_materializations").totalValue, 1);
	EXPECT_TRUE(logical.visitEdgesUntil(snapshot, 10U, [](const LogicalGraphEdge &edge) {
		return edge.edge_id == 2001U;
	}));
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("native_graph.read.full_materializations").totalValue, 1);
	EXPECT_THROW(static_cast<void>(logical.readNode(snapshot, 0U)), std::exception);
	EXPECT_THROW(static_cast<void>(logical.readEdge(snapshot, 0U)), std::exception);
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.full_materializations"));
	EXPECT_EQ(logical.readNodesShapedSorted(snapshot, nodes, {}).size(), 1U);
	EXPECT_EQ(logical.readEdgesShapedSorted(snapshot, edges, {}).size(), 1U);
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.full_materializations"));
	NativeReadSession session;
	static_cast<void>(pages.readNodeRecord(snapshot, 1001U, &session));
	static_cast<void>(pages.readEdgeTopology(snapshot, 2001U, &session));
	const auto firstDecode = PerfTrace::snapshotAndReset();
	ASSERT_TRUE(firstDecode.contains("native_graph.read.records_decoded"));
	EXPECT_GT(firstDecode.at("native_graph.read.records_decoded").totalValue, 0);
	static_cast<void>(pages.readNodeRecord(snapshot, 1001U, &session));
	static_cast<void>(pages.readEdgeTopology(snapshot, 2001U, &session));
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.records_decoded"));
}

TEST(RelocatableEntityPageTest, DecodeCountersCountSuccessfulEntityCodecsWithoutCountingEncodeOrFailure) {
	using graph::debug::PerfTrace;
	struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::reset(); } } cleanup;
	PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(true); PerfTrace::reset();
	const auto node = NodeEntityRecordCodec::encode(NodeEntityRecord{});
	const EdgeEntityRecord edge{1U, 2U, 0U};
	const auto encodedEdge = EdgeEntityRecordCodec::encode(edge);
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.records_decoded"));
	EXPECT_EQ(NodeEntityRecordCodec::decode(node), NodeEntityRecord{});
	EXPECT_EQ(EdgeEntityRecordCodec::decode(encodedEdge), edge);
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("native_graph.read.records_decoded").totalValue, 2);
	EXPECT_THROW(static_cast<void>(NodeEntityRecordCodec::decode({})), std::exception);
	EXPECT_THROW(static_cast<void>(EdgeEntityRecordCodec::decode({})), std::exception);
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.records_decoded"));
}
