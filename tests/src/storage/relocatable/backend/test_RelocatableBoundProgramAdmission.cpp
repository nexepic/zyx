#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "CypherParserImpl.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
using namespace relocatable_storage_backend_test;
namespace {
struct BindingTraceScope {
    bool previous = graph::debug::PerfTrace::areValuesEnabled();
    BindingTraceScope() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true); }
    ~BindingTraceScope() { graph::debug::PerfTrace::setValueCollectionEnabled(previous); }
};
}
TEST_F(RelocatableStorageBackendTest, OptionalBindingRefundsDescriptorsAndLeavesSmallBudgetForPresenceProbe) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(std::move(options)); backend.open();
    (void)backend.createLogicalNodes({{{"BindingAdmission"}, {{"payload", PropertyValue(std::string(8192U, 'x'))}}},
                                      {{"BindingAdmission"}, {}}});
    graph::parser::cypher::CypherParserImpl parser;
    auto plan = parser.parseToLogical("MATCH (n:BindingAdmission) RETURN count(n.payload) AS count");
    execution::LogicalPlanReadRuntime runtime(backend);
    execution::LogicalPlanReadRuntimeBounds bounds{
        .maximum_node_records = 4U, .maximum_node_page_records = 2U,
        .maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 4U,
        .maximum_edge_records = 4U, .maximum_adjacency_page_records = 2U,
        .maximum_property_entries = 2U, .maximum_rows = 4U,
        .maximum_distinct_rows = 4U, .maximum_materialized_bytes = 65536U};
    for (const size_t bytes : {1024U, 65536U, 1024U}) {
        BindingTraceScope traceScope; bounds.maximum_materialized_bytes = bytes;
        const auto result = runtime.executeToQueryResult(*plan.root, bounds);
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(int64_t{1}));
        const auto trace = graph::debug::PerfTrace::snapshotAndReset();
        EXPECT_EQ(trace.contains("logical_plan.bound_program.quota_fallbacks"), bytes == 1024U);
        EXPECT_EQ(trace.contains("logical_plan.bound_program.admitted_bytes"), bytes == 65536U);
        EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
    }
    backend.close();
}
TEST_F(RelocatableStorageBackendTest, BindingFallbackDoesNotWaiveMandatoryRowQuotaAndCanRetry) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(std::move(options)); backend.open();
    graph::parser::cypher::CypherParserImpl parser; auto plan = parser.parseToLogical("RETURN 1 AS value");
    execution::LogicalPlanReadRuntime runtime(backend); execution::LogicalPlanReadRuntimeBounds bounds{
        .maximum_node_records = 4U, .maximum_node_page_records = 2U,
        .maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 4U,
        .maximum_edge_records = 4U, .maximum_adjacency_page_records = 2U,
        .maximum_property_entries = 2U, .maximum_rows = 4U,
        .maximum_distinct_rows = 4U, .maximum_materialized_bytes = 65536U};
    bounds.maximum_materialized_bytes = 1U;
    EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds)), execution::MaterializedByteLimitExceeded);
    bounds.maximum_materialized_bytes = 65536U;
    const auto result = runtime.executeToQueryResult(*plan.root, bounds);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
    backend.close();
}
