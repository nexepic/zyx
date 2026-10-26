#include <array>
#include <limits>
#include "storage/relocatable/entity/PageBackedGraphReadAdapterTestFixture.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"

using namespace graph::storage;

namespace {
class WideIdPropertyReadTest : public graph::storage::test::PageBackedGraphReadAdapterTest {
protected:
    std::unique_ptr<RelocatableMetadataStore> metadata;
    std::unique_ptr<EntityPageStore> entities;
    std::unique_ptr<SlottedPagePool> pages;
    std::unique_ptr<EntityLabelSetStore> labels;
    std::unique_ptr<PropertyCollectionRootStore> properties;
    std::unique_ptr<BTreePageStore> indexes;
    std::unique_ptr<PageBackedGraphReadAdapter> graph;
    static constexpr uint64_t high = UINT64_C(1) << 63U;
    static constexpr std::array<uint64_t, 3> nodes{1, high, UINT64_MAX};
    void SetUp() override {
        PageBackedGraphReadAdapterTest::SetUp();
        Superblock root; root.database_id = DatabaseId(12345); root.generation = Generation(1);
        root.checkpoint_lsn = LogSequenceNumber(1);
        metadata = RelocatableMetadataStore::initializeNew(io, root);
        entities = std::make_unique<EntityPageStore>(io, *metadata);
        pages = std::make_unique<SlottedPagePool>(io, *metadata);
        labels = std::make_unique<EntityLabelSetStore>(io, *metadata, *pages);
        properties = std::make_unique<PropertyCollectionRootStore>(*pages, *metadata);
        indexes = std::make_unique<BTreePageStore>(*pages, *metadata);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const EntityPageNewPoolRequest pool{SegmentId(100 + i), 901, Generation(1), PageId(200 + i),
                PhysicalSlotId(20 + i), PhysicalSlotId(20 + i)};
            ASSERT_TRUE(entities->allocateNode(pool, nodes[i], {}, LogSequenceNumber(2 + i)));
        }
        const std::array<uint64_t, 2> edges{1, UINT64_MAX};
        for (size_t i = 0; i < edges.size(); ++i) {
            const EntityPageNewPoolRequest pool{SegmentId(110 + i), 902, Generation(1), PageId(210 + i),
                PhysicalSlotId(30 + i), PhysicalSlotId(30 + i)};
            EdgeEntityRecord edge; edge.source_node_id = 1; edge.target_node_id = high; edge.type_id = 3;
            ASSERT_TRUE(entities->allocateEdge(pool, edges[i], edge, LogSequenceNumber(5 + i)));
        }
        const std::array roots{
            PropertyCollectionRoot{1, graph::Node::typeId, {{1, graph::PropertyValue(int64_t{7})}}},
            PropertyCollectionRoot{1, graph::Edge::typeId, {{1, graph::PropertyValue(int64_t{9})}}}};
        static_cast<void>(graph::storage::test::publishPropertyRoots(*entities, *properties, *metadata, roots,
            {PhysicalSlotId(50), PhysicalSlotId(70), 903}));
        graph = std::make_unique<PageBackedGraphReadAdapter>(*metadata, *entities, *labels, *properties, *indexes);
    }
    void TearDown() override {
        graph.reset(); indexes.reset(); properties.reset(); labels.reset(); pages.reset(); entities.reset(); metadata.reset();
        PageBackedGraphReadAdapterTest::TearDown();
    }
};
}

TEST_F(WideIdPropertyReadTest, TaggedPropertyDomainDoesNotHideWidePrimaryNodesOrEdges) {
    auto snapshot = metadata->openSnapshot();
    EXPECT_EQ(graph->readNode(snapshot, UINT64_MAX).node_id, UINT64_MAX);
    EXPECT_TRUE(graph->readNode(snapshot, UINT64_MAX).properties.empty());
    EXPECT_TRUE(graph->readEdge(snapshot, UINT64_MAX).properties.empty());
    EXPECT_FALSE(graph->readNodeProperty(snapshot, high, 1));
    EXPECT_FALSE(graph->readEdgeProperty(snapshot, UINT64_MAX, 1));
    EXPECT_FALSE(graph->nodePropertyExists(snapshot, UINT64_MAX, 1));
    EXPECT_FALSE(graph->edgePropertyExists(snapshot, UINT64_MAX, 1));
    EXPECT_EQ(graph->nodePropertyCount(snapshot, UINT64_MAX), 0U);
    const auto counts = graph->nodePropertyCountsSorted(snapshot, nodes);
    ASSERT_EQ(counts.size(), 3U); EXPECT_EQ(counts[0], 1U); EXPECT_EQ(counts[1], 0U); EXPECT_EQ(counts[2], 0U);
    const auto all = graph->readNodePropertiesSorted(snapshot, nodes);
    ASSERT_EQ(all.size(), 3U); EXPECT_EQ(all[0].size(), 1U); EXPECT_TRUE(all[1].empty()); EXPECT_TRUE(all[2].empty());
    const auto selected = graph->readNodePropertiesSorted(snapshot, nodes, 1);
    ASSERT_EQ(selected.size(), 3U); ASSERT_TRUE(selected[0]);
    EXPECT_EQ(*selected[0], graph::PropertyValue(int64_t{7})); EXPECT_FALSE(selected[1]); EXPECT_FALSE(selected[2]);
    const std::array<uint32_t, 2> tokens{1, 2};
    const auto columns = graph->readNodePropertyColumnsSorted(snapshot, nodes, tokens);
    ASSERT_EQ(columns.size(), 2U); EXPECT_EQ(columns[0][0], graph::PropertyValue(int64_t{7}));
    EXPECT_FALSE(columns[0][1]); EXPECT_FALSE(columns[0][2]); EXPECT_FALSE(columns[1][2]);
    const auto combined = graph->readNodePropertyColumnsAndCountsSorted(snapshot, nodes, tokens);
    EXPECT_EQ(combined.second, counts); EXPECT_EQ(combined.first, columns);
    size_t cells = 0, present = 0;
    graph->readNodePropertyCellsSorted(snapshot, nodes, tokens,
        [&](size_t row, size_t column, std::optional<graph::PropertyValue> value) {
            ++cells; if (value) { ++present; EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U); }
        }, nullptr, nullptr);
    EXPECT_EQ(cells, 6U); EXPECT_EQ(present, 1U);
    const std::array<uint64_t, 2> edges{1, UINT64_MAX};
    const auto edgeValues = graph->readEdgePropertiesSorted(snapshot, edges, 1);
    ASSERT_EQ(edgeValues.size(), 2U); EXPECT_EQ(edgeValues[0], graph::PropertyValue(int64_t{9})); EXPECT_FALSE(edgeValues[1]);
    const auto edgeColumns = graph->readEdgePropertyColumnsSorted(snapshot, edges, tokens);
    EXPECT_EQ(edgeColumns[0][0], graph::PropertyValue(int64_t{9})); EXPECT_FALSE(edgeColumns[0][1]);
    EXPECT_TRUE(graph->readEdgePropertiesSorted(snapshot, edges)[1].empty());
}

TEST_F(WideIdPropertyReadTest, WideAbsenceKeepsSnapshotOrderingAndTokenValidation) {
    auto snapshot = metadata->openSnapshot();
    auto foreign = RelocatableMetadataStore::openReadOnly(io); auto other = foreign->openSnapshot();
    EXPECT_THROW(static_cast<void>(graph->readNodeProperty(other, UINT64_MAX, 1)), FormatException);
    EXPECT_THROW(static_cast<void>(graph->readEdgeProperty(other, UINT64_MAX, 1)), FormatException);
    EXPECT_THROW(static_cast<void>(graph->nodePropertyCount(other, UINT64_MAX)), FormatException);
    EXPECT_THROW(static_cast<void>(graph->readNodeProperty(snapshot, 0, 1)), FormatException);
    EXPECT_THROW(static_cast<void>(graph->readNodeProperty(snapshot, UINT64_MAX, 0)), FormatException);
    const std::array<uint32_t, 1> tokens{1}; const std::array<uint64_t, 3> bad{1, UINT64_MAX, UINT64_MAX};
    size_t emitted = 0;
    const std::function<void(size_t, size_t, std::optional<graph::PropertyValue>)> sink =
        [&](size_t, size_t, auto) { ++emitted; };
    EXPECT_THROW(graph->readNodePropertyCellsSorted(snapshot, bad, tokens, sink, nullptr, nullptr), FormatException);
    EXPECT_EQ(emitted, 0U);
    const std::array<uint64_t, 1> terminal{UINT64_MAX}; const std::array<uint32_t, 1> invalidToken{0};
    EXPECT_THROW(graph->readNodePropertyCellsSorted(snapshot, terminal, invalidToken, sink, nullptr, nullptr), FormatException);
    EXPECT_EQ(emitted, 0U);
    EXPECT_THROW(static_cast<void>(graph->readNodePropertyColumnsAndCountsSorted(snapshot, terminal, invalidToken)), FormatException);
}
