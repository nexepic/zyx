#include "storage/relocatable/property/HybridPropertyQueryParityTestFixture.hpp"
using namespace hybrid_property_query_parity_test;
namespace graph::query::execution { extern thread_local bool test_enable_streaming_count; }
struct EnableStreamingPersistence { EnableStreamingPersistence() { graph::query::execution::test_enable_streaming_count = true; } ~EnableStreamingPersistence() { graph::query::execution::test_enable_streaming_count = false; } };

TEST_F(HybridPropertyQueryParityTest, StreamingCountPreservesPersistentLayoutUpdatesAndReopenedResults) {
    if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED) GTEST_SKIP() << "ROW fixture requires native initialization; SCAN uses its offline image fixture";
    EnableStreamingPersistence enable;
    seed(false);
    const auto check = [&](int64_t expected) {
        auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        RelocatableStorageBackend backend(options); backend.open();
        query::execution::LogicalPlanReadRuntime runtime(backend);
        query::execution::LogicalPlanReadRuntimeBounds bounds;
        bounds.maximum_node_records = 128; bounds.maximum_rows = 1024;
        bounds.maximum_edge_records = 256; bounds.maximum_edges_per_node = 128;
        bounds.maximum_adjacency_page_records = 16; bounds.maximum_distinct_rows = 1024;
        bounds.maximum_node_page_records = 16; bounds.maximum_metadata_pages_per_node_page = 8192;
        bounds.maximum_property_entries = 16;
        parser::cypher::CypherParserImpl parser;
        auto plan = parser.parseToLogical("MATCH (n) WHERE n.age >= 20 RETURN count(n) AS count");
        int64_t sourceRows = 0, materializedRows = 0;
        struct Work { int64_t &source, &materialized; } work{sourceRows, materializedRows};
        debug::PerfTrace::ScopedObserver observe(&work, [](void *ctx, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(ctx);
            if (key == "logical_plan.streaming_count.source_rows") w.source += value;
            if (key == "logical_plan.direct_property.rows") w.materialized += value;
        });
        const auto result = runtime.executeToQueryResult(*plan.root, bounds);
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows()[0].at("count").asPrimitive(), PropertyValue(expected));
        EXPECT_EQ(sourceRows, 64); EXPECT_EQ(materializedRows, 0);
        backend.close();
    };
    check(44);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) convert(EntityPropertyOwnerKind::EPOK_NODE);
    if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
    check(44);
    setAge(100); check(45);
    setAge(0); check(44);
    convert(EntityPropertyOwnerKind::EPOK_NODE, true); check(44);
}

TEST_F(HybridPropertyQueryParityTest, StreamingCountKeepsOldSnapshotAcrossTypeNullRemovalAndDeletion) {
    if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED) GTEST_SKIP() << "ROW fixture requires native initialization; SCAN uses its offline image fixture";
    EnableStreamingPersistence enable;
    seed(false);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) convert(EntityPropertyOwnerKind::EPOK_NODE);
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    query::execution::LogicalPlanReadRuntime runtime(backend);
    query::execution::LogicalPlanReadRuntimeBounds bounds;
    bounds.maximum_node_records = 128; bounds.maximum_rows = 1024;
    bounds.maximum_edge_records = 256; bounds.maximum_edges_per_node = 128;
    bounds.maximum_adjacency_page_records = 16; bounds.maximum_distinct_rows = 1024;
    bounds.maximum_node_page_records = 16; bounds.maximum_metadata_pages_per_node_page = 8192;
    bounds.maximum_property_entries = 16;
    parser::cypher::CypherParserImpl parser;
    auto plan = parser.parseToLogical("MATCH (n) WHERE n.age >= 20 RETURN count(n) AS count");
    const auto count = [&](LogicalGraphReadSnapshot &snapshot) {
        int64_t value = -1;
        (void)runtime.executeWithSnapshot(snapshot, *plan.root, bounds, [&](const auto &row) {
            value = std::get<int64_t>(std::get<PropertyValue>(row.values.at("count")).getVariant()); return true;
        }, {});
        return value;
    };
    ASSERT_EQ(count(*old), 44);
    for (const auto &value : {PropertyValue(100), PropertyValue(100.0), PropertyValue("text")}) {
        applyPublicNativePatch(backend, {{"age", value}});
        auto current = backend.openLogicalGraphReadSnapshot();
        const auto expected = value.getType() != PropertyType::NULL_TYPE && value >= PropertyValue(20) ? 45 : 44;
        EXPECT_EQ(count(*current), expected); EXPECT_EQ(count(*old), 44);
    }
    applyPublicNativePatch(backend, {}, {"age"});
    { auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(count(*current), 44); }
    backend.deleteLogicalNodes({{64, false, 0}});
    { auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(count(*current), 43); }
    EXPECT_EQ(count(*old), 44); old.reset(); backend.close();
}
