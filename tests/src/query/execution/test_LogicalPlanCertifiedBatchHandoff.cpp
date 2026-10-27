#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include "graph/storage/LogicalGraphNodeScan.hpp"
#include "graph/query/QueryGuard.hpp"

namespace graph::query::execution {
namespace {
using namespace direct_edge_test;
struct BatchState {
    std::vector<uint64_t> ids;
    size_t source_calls = 0, batches = 0;
    bool enabled = true;
};
class BatchSnapshot final : public DirectEdgeSnapshot, public storage::LogicalGraphNodeScanProvider {
public:
    BatchSnapshot(DirectEdgeGraph &graph, BatchState &state) : DirectEdgeSnapshot(graph), state_(state) {}
    bool hasImmutableReadIdentity() const noexcept override { return true; }
    std::optional<bool> tryVisitNodeScanBatches(size_t maximum, size_t batchRows,
        const std::vector<std::string> &, std::span<const std::string_view> keys,
        const NodeScanBatchVisitor &visitor, const std::function<void(size_t)> &admit) const override {
        if (!state_.enabled) return std::nullopt;
        admit(3U * sizeof(uint64_t));
        std::array<uint64_t, 3> reused{};
        const size_t end = (std::min)(maximum, state_.ids.size());
        for (size_t start = 0; start < end;) {
            const size_t n = (std::min)({size_t{3}, batchRows, end - start});
            std::copy_n(state_.ids.begin() + start, n, reused.begin());
            NodeScanPropertySource source = [&](std::span<const uint64_t> requested,
                const PropertyCellSink &sink, const storage::PropertyColumnBatchReadSinks &,
                const std::function<void(size_t)> &admission) {
                ++state_.source_calls;
                for (auto id : requested)
                    if (std::find(reused.begin(), reused.begin() + n, id) == reused.begin() + n)
                        throw std::runtime_error("source requested IDs outside current borrow");
                readNodePropertyCellsSortedWithAdmission(requested, keys, sink, admission);
            };
            ++state_.batches;
            const bool more = visitor(std::span(reused).first(n), n, source);
            reused.fill(0); // A later consumer must never retain the callback's IDs.
            if (!more) return false;
            start += n;
        }
        return true;
    }
private:
    BatchState &state_;
};
class BatchBackend final : public LogicalGraphReadBackend {
public:
    BatchBackend(DirectEdgeGraph &graph, BatchState &state) : graph_(graph), state_(state) {}
    std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
        return std::make_unique<BatchSnapshot>(graph_, state_);
    }
private:
    DirectEdgeGraph &graph_; BatchState &state_;
};
std::shared_ptr<expressions::Expression> rank() {
    return std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
}
TEST(LogicalPlanCertifiedBatchHandoffTest, BorrowsReusedBatchesAndPreservesPhysicalFloatingOrder) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    graph.nodes.at(1).properties = {{"rank", PropertyValue(1e16)}};
    graph.nodes.at(2).properties = {{"rank", PropertyValue(-1e16)}};
    graph.nodes.at(3).properties = {{"rank", PropertyValue(1.0)}};
    graph.nodes.emplace(4, LogicalGraphNode{4, {}, {{"rank", PropertyValue()}}});
    graph.nodes.emplace(5, LogicalGraphNode{5, {}, {}});
    BatchState state{{3, 1, 2, 5, 4}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
        {{"count", nullptr, "rows"}, {"count", rank(), "present"}, {"sum", rank(), "total"}});
    const auto result = runtime.executeToQueryResult(aggregate, directBounds());
    ASSERT_EQ(result.rowCount(), 1U);
    const auto &row = result.getRows().front();
    EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(5));
    EXPECT_EQ(row.at("present").asPrimitive(), PropertyValue(3));
    EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(0.0));
    EXPECT_EQ(state.batches, 2U); EXPECT_EQ(state.source_calls, 2U);
    EXPECT_TRUE(graph.node_read_ids.empty());
}
TEST(LogicalPlanCertifiedBatchHandoffTest, InvalidIdsFailBeforeSourceAccessAndNeverReplay) {
    for (const auto &ids : {std::vector<uint64_t>{1, 0, 2}, std::vector<uint64_t>{2, 1, 2}}) {
        auto graph = directGraph(); graph.enable_direct_property_cells = true;
        BatchState state{ids}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
        logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", rank(), "total"}});
        EXPECT_THROW((void)runtime.executeToQueryResult(aggregate, directBounds()), std::runtime_error);
        EXPECT_EQ(state.source_calls, 0U); EXPECT_EQ(state.batches, 1U);
    }
}
TEST(LogicalPlanCertifiedBatchHandoffTest, ProviderDeclineRetainsOrdinaryReferencePath) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    BatchState state; state.enabled = false;
    BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", rank(), "total"}});
    const auto result = runtime.executeToQueryResult(aggregate, directBounds());
    ASSERT_EQ(result.rowCount(), 1U); EXPECT_EQ(result.getRows()[0].at("total").asPrimitive(), PropertyValue(6));
    EXPECT_EQ(state.batches, 0U); EXPECT_GT(graph.direct_property_batches, 0U);
}
TEST(LogicalPlanCertifiedBatchHandoffTest, GuardFailureDuringProductionIsNotRetried) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    BatchState state{{3, 1, 2}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"sum", rank(), "total"}});
    bool fired = false;
    QueryGuard guard(0, 1, [&] {
        if (!fired && state.source_calls) { fired = true; throw MaterializedByteLimitExceeded(); }
        return size_t{0};
    });
    QueryContext context; context.guard = &guard;
    EXPECT_THROW((void)runtime.executeToQueryResult(aggregate, directBounds(), context), MaterializedByteLimitExceeded);
    EXPECT_TRUE(fired); EXPECT_EQ(state.source_calls, 1U); EXPECT_EQ(state.batches, 1U);
}
TEST(LogicalPlanCertifiedBatchHandoffTest, PrivateBytePressureSplitsWithoutDuplicateCounts) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    for (auto &[id, node] : graph.nodes) node.properties = {{"rank", PropertyValue(std::string(8000, 'x'))}};
    BatchState state{{3, 1, 2}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {rank()},
        {{"count", nullptr, "rows"}}, {"key"});
    auto bounds = directBounds(); bounds.maximum_materialized_bytes = 48U * 1024U;
    const auto result = runtime.executeToQueryResult(aggregate, bounds);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(3));
    EXPECT_EQ(result.getRows()[0].at("key").asPrimitive(), PropertyValue(std::string(8000, 'x')));
    EXPECT_GT(state.source_calls, 1U); EXPECT_EQ(state.batches, 1U);
}
TEST(LogicalPlanCertifiedBatchHandoffTest, RecordBoundDoesNotFetchNextSourceBatch) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    BatchState state{{3, 1, 2, 1, 2, 3}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
        {{"sum", rank(), "value"}, {"count", nullptr, "rows"}});
    auto bounds = directBounds(); bounds.maximum_node_records = 3;
    const auto result = runtime.executeToQueryResult(aggregate, bounds);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(6));
    EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(3));
    EXPECT_EQ(state.batches, 1U); EXPECT_EQ(state.source_calls, 1U);
}

} // namespace
} // namespace graph::query::execution
