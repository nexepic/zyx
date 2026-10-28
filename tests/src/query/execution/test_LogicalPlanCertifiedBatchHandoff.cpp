#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include "graph/storage/LogicalGraphNodeScan.hpp"
#include "graph/query/QueryGuard.hpp"
#include <chrono>
#include <iostream>

namespace graph::query::execution {
extern thread_local bool test_disable_streaming_count;
extern thread_local bool test_enable_streaming_count;
namespace {
using namespace direct_edge_test;
struct EnableStreaming { EnableStreaming() { test_enable_streaming_count = true; } ~EnableStreaming() { test_enable_streaming_count = false; } };
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

TEST(LogicalPlanCertifiedBatchHandoffTest, FilterCountConsumesSourceWithoutIntermediateColumns) {
    EnableStreaming enable;
    for (bool enabled : {false, true}) {
        auto graph = directGraph(); graph.enable_direct_property_cells = true;
        graph.nodes.emplace(4, LogicalGraphNode{4, {}, {{"rank", PropertyValue()}}});
        graph.nodes.emplace(5, LogicalGraphNode{5, {}, {}});
        BatchState state{{3, 1, 2, 5, 4}}; state.enabled = enabled;
        BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
        auto scan = std::make_unique<logical::LogicalNodeScan>("n");
        scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), false, true}});
        logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
        int64_t streaming = 0, materialized = 0;
        struct Work { int64_t &streaming, &materialized; } work{streaming, materialized};
        debug::PerfTrace::ScopedObserver observe(&work, [](void *ctx, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(ctx);
            if (key == "logical_plan.streaming_count.source_rows") w.streaming += value;
            if (key == "logical_plan.direct_property.rows") w.materialized += value;
        });
        const auto result = runtime.executeToQueryResult(aggregate, directBounds());
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(2));
        EXPECT_EQ(streaming, enabled ? 5 : 0);
        EXPECT_EQ(materialized, enabled ? 0 : 5);
    }
}

TEST(LogicalPlanCertifiedBatchHandoffTest, StreamingCountGuardFailureIsNotRetried) {
    EnableStreaming enable;
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    BatchState state{{3, 1, 2}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    auto scan = std::make_unique<logical::LogicalNodeScan>("n");
    scan->setRangePredicates({{"rank", PropertyValue(0), PropertyValue{}, false, true}});
    logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
    bool fired = false;
    QueryGuard guard(0, 1, [&] {
        if (!fired && state.source_calls) { fired = true; throw MaterializedByteLimitExceeded(); }
        return size_t{0};
    });
    QueryContext context; context.guard = &guard;
    EXPECT_THROW((void)runtime.executeToQueryResult(aggregate, directBounds(), context), MaterializedByteLimitExceeded);
    EXPECT_TRUE(fired); EXPECT_EQ(state.source_calls, 1U); EXPECT_EQ(state.batches, 1U);
}
TEST(LogicalPlanCertifiedBatchHandoffTest, StreamingCountPressureDiscardsPrivatePrefixBeforeSplit) {
    EnableStreaming enable;
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    for (auto &[id, node] : graph.nodes) node.properties = {{"rank", PropertyValue(std::string(8000, 'x'))}};
    BatchState state{{3, 1, 2}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    auto scan = std::make_unique<logical::LogicalNodeScan>("n");
    scan->setRangePredicates({{"rank", PropertyValue{}, PropertyValue{}, true, true}});
    logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
    auto bounds = directBounds(); bounds.maximum_materialized_bytes = 24U * 1024U;
    const auto result = runtime.executeToQueryResult(aggregate, bounds);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(3));
    EXPECT_GT(state.source_calls, 1U); EXPECT_EQ(state.batches, 1U);
}

TEST(LogicalPlanCertifiedBatchHandoffTest, StreamingCountMatchesFallbackAcrossSelectionRatesAndMixedValues) {
    EnableStreaming enable;
    for (int64_t minimum : {-10, 2, 10}) {
        for (bool enabled : {false, true}) {
            auto graph = directGraph(); graph.enable_direct_property_cells = true;
            graph.nodes.emplace(4, LogicalGraphNode{4, {}, {{"rank", PropertyValue()}}});
            graph.nodes.emplace(5, LogicalGraphNode{5, {}, {}});
            graph.nodes.emplace(6, LogicalGraphNode{6, {}, {{"rank", PropertyValue(2.5)}}});
            BatchState state{{6, 4, 3, 1, 5, 2}}; state.enabled = enabled;
            BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
            auto scan = std::make_unique<logical::LogicalNodeScan>("n");
            scan->setRangePredicates({{"rank", PropertyValue(minimum), PropertyValue{}, true, true}});
            logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
            size_t expected = 0;
            for (const auto &[id, node] : graph.nodes) {
                auto it = std::ranges::find(node.properties, "rank", &storage::LogicalGraphPropertyEntry::key);
                if (it != node.properties.end() && it->value.getType() != PropertyType::NULL_TYPE &&
                    it->value >= PropertyValue(minimum)) ++expected;
            }
            const auto result = runtime.executeToQueryResult(aggregate, directBounds());
            ASSERT_EQ(result.rowCount(), 1U);
            EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(static_cast<int64_t>(expected)));
        }
    }
}

TEST(LogicalPlanCertifiedBatchHandoffTest, DISABLED_BoundedSamePathCountFusionProbe) {
    EnableStreaming enable;
    struct Restore { ~Restore() { test_disable_streaming_count = false; } } restore;
    constexpr size_t seed = 20261027;
    for (size_t count : {size_t{96}, size_t{384}}) for (bool mixed : {false, true})
    for (int64_t threshold : {int64_t{101}, int64_t{90}, int64_t{10}}) {
        DirectEdgeGraph graph; graph.enable_direct_property_cells = true;
        BatchState state;
        for (size_t i = 0; i < count; ++i) {
            const int64_t value = static_cast<int64_t>((i * 37U + seed) % 100U);
            std::vector<storage::LogicalGraphPropertyEntry> properties;
            if (!mixed || i % 8U != 0U) {
                if (mixed && i % 8U == 1U) properties.push_back({"rank", PropertyValue()});
                else if (mixed && i % 8U == 2U) properties.push_back({"rank", PropertyValue(double(value))});
                else properties.push_back({"rank", PropertyValue(value)});
            }
            const auto id = i + 1U;
            graph.nodes.emplace(id, LogicalGraphNode{id, {}, std::move(properties)});
            state.ids.push_back(id);
        }
        size_t expected = 0;
        for (const auto &[id, node] : graph.nodes) for (const auto &property : node.properties)
            if (property.key == "rank" && property.value.getType() != PropertyType::NULL_TYPE && property.value >= PropertyValue(threshold)) ++expected;
        BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
        auto scan = std::make_unique<logical::LogicalNodeScan>("n");
        scan->setRangePredicates({{"rank", PropertyValue(threshold), PropertyValue{}, true, true}});
        logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
        auto bounds = directBounds(); bounds.maximum_node_records = count + 1U; bounds.maximum_rows = count + 1U;
        bounds.maximum_distinct_rows = count + 1U; bounds.maximum_materialized_bytes = 64U * 1024U * 1024U;
        const auto execute = [&](bool disabled) {
            test_disable_streaming_count = disabled;
            auto result = runtime.executeToQueryResult(aggregate, bounds);
            if (result.rowCount() != 1 || result.getRows()[0].at("rows").asPrimitive() != PropertyValue(static_cast<int64_t>(expected)))
                throw std::runtime_error("count fusion probe oracle mismatch");
        };
        for (int iteration = 0; iteration < 9; ++iteration) for (int arm = 0; arm < 2; ++arm) {
            bool disabled = ((arm + iteration + int(mixed) + int(threshold)) & 1) != 0;
            const auto start = std::chrono::steady_clock::now(); execute(disabled);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            if (iteration >= 3) std::cout << "COUNT_PROBE rows=" << count << " mixed=" << mixed << " threshold=" << threshold
                << " disabled=" << disabled << " expected=" << expected << " iteration=" << iteration - 3 << " ns=" << elapsed << '\n';
        }
        for (bool disabled : {false, true}) {
            struct Counters { int64_t stream = 0, materialized = 0; } counters;
            graph::debug::PerfTrace::ScopedObserver observer(&counters, [](void *ctx, std::string_view key, int64_t value, bool) noexcept {
                auto &c = *static_cast<Counters *>(ctx);
                if (key == "logical_plan.streaming_count.source_rows") c.stream += value;
                if (key == "logical_plan.direct_property.rows") c.materialized += value;
            });
            execute(disabled);
            std::cout << "COUNT_WORK rows=" << count << " mixed=" << mixed << " threshold=" << threshold << " disabled=" << disabled
                << " stream_rows=" << counters.stream << " materialized_rows=" << counters.materialized << '\n';
        }
    }
}

TEST(LogicalPlanCertifiedBatchHandoffTest, StreamingCountIsDisabledByDefault) {
    auto graph = directGraph(); graph.enable_direct_property_cells = true;
    BatchState state{{1, 2, 3}}; BatchBackend backend(graph, state); LogicalPlanReadRuntime runtime(backend);
    auto scan = std::make_unique<logical::LogicalNodeScan>("n");
    scan->setRangePredicates({{"rank", PropertyValue(1), PropertyValue(3), false, true}});
    logical::LogicalAggregate aggregate(std::move(scan), {}, {{"count", nullptr, "rows"}});
    int64_t streaming = 0, direct = 0;
    struct Counters { int64_t &streaming, &direct; } counters{streaming, direct};
    debug::PerfTrace::ScopedObserver observe(&counters, [](void *ctx, std::string_view key, int64_t v, bool) noexcept {
        auto &c = *static_cast<Counters *>(ctx);
        if (key == "logical_plan.streaming_count.source_rows") c.streaming += v;
        if (key == "logical_plan.direct_property.rows") c.direct += v;
    });
    const auto result = runtime.executeToQueryResult(aggregate, directBounds());
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows()[0].at("rows").asPrimitive(), PropertyValue(2));
    EXPECT_EQ(streaming, 0); EXPECT_EQ(direct, 3);
}

} // namespace
} // namespace graph::query::execution
