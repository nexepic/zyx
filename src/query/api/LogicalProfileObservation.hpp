#pragma once

#include <array>
#include <algorithm>
#include <limits>
#include <string_view>
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::detail {
inline std::string_view logicalExecutionContract(logical::LogicalOpType type) noexcept {
    using T = logical::LogicalOpType;
    switch (type) {
        case T::LOP_NODE_SCAN: return "runtime selects index/label/full scan; direct cells when eligible, otherwise bounded rows; estimates unavailable before snapshot";
        case T::LOP_FILTER: return "literal supported predicates may consume direct cells; residual expressions retain bounded evaluator";
        case T::LOP_AGGREGATE: return "eligible property aggregates consume direct cells; complex expressions use bounded owning adapter; no general group spill";
        case T::LOP_SORT: return "bounded owning keys; Top-K when eligible, otherwise memory/internal-file sort subject to scratch permission";
        case T::LOP_JOIN: return "bounded owning build rows and keys; no hash-join spill";
        case T::LOP_TRAVERSAL:
        case T::LOP_VAR_LENGTH_TRAVERSAL: return "bounded topology continuation and property gather; authoritative Edge checks retained";
        case T::LOP_UNWIND:
        case T::LOP_CALL_SUBQUERY:
        case T::LOP_CALL_PROCEDURE: return "bounded owning adapter preserves correlation, multiplicity and side effects";
        default: return "bound runtime operator; actual work is available only after PROFILE execution";
    }
}

// One synchronous query, fixed metric registry, no process trace reset/toggle.
// Unavailable metrics are omitted; zero observed work remains explicit zero.
struct LogicalProfileMetric { std::string_view key; bool duration = false; bool peak = false; };
class LogicalProfileObservation final {
    using Metric = LogicalProfileMetric;
    inline static constexpr auto metrics_ = std::to_array<Metric>({
        {"logical_plan.direct_property.batches"}, {"logical_plan.direct_property.rows"},
        {"logical_plan.direct_property.rejected"}, {"logical_plan.aggregate.direct_input_rows"}, {"logical_plan.aggregate.direct_filter_rejected"},
        {"logical_plan.index_candidate.materialized_bytes"}, {"logical_plan.batch.rows"}, {"logical_plan.batch.materialized_bytes"},
        {"logical_plan.consumer_adapter.sort.calls"}, {"logical_plan.consumer_adapter.join.calls"},
        {"logical_plan.consumer_adapter.optional.calls"}, {"logical_plan.consumer_adapter.subquery.calls"},
        {"logical_plan.consumer_adapter.unwind.calls"}, {"logical_plan.consumer_adapter.expand.calls"},
        {"logical_plan.consumer_adapter.variable_expand.calls"}, {"logical_plan.consumer_adapter.csv.calls"},
        {"logical_plan.consumer_adapter.union.calls"}, {"logical_plan.sort.spilled"},
        {"logical_plan.sort.topk_input_rows"}, {"logical_plan.sort.topk_retained_rows"},
        {"logical_plan.materialized_budget_peak_bytes", false, true},
        {"native_graph.read.property_selected_decode"},
        {"native_graph.read.directory_range_cursor_metadata_pages"},
        {"native_graph.read.directory_range_cursor_root_descents"},
        {"native_graph.read.decoded_metadata_cache_hits"},
        {"native_graph.read.decoded_metadata_copy_fixed_bytes"},
        {"native_graph.read.decoded_metadata_borrow_fixed_bytes"},
        {"storage.checksum.bytes"}, {"storage.buffer_pool.physical_read_bytes"},
        {"storage_io.file_size_calls"}, {"storage_io.completed_write_bytes"},
        {"storage_io.completed_write_calls"},
        {"native_graph.write.property_pool_catalog_scans"}, {"native_graph.write.property_plan_owners"},
        {"native_graph.write.property_plan_owned_payload_bytes"},
        {"native_graph.write.property_plan_coalesced_members"},
        {"native_graph.write.property_plan_replacement_images"},
        {"native_graph.write.property_plan_replacement_payload_bytes"},
        {"native_graph.write.id_reservation_calls"}, {"native_graph.write.id_reservation_publications"}, {"native_graph.write.id_lease_cache_hits"}, {"native_graph.write.graph_commit_calls"},
        {"native_graph.write.maintenance_reclaim_calls"}, {"storage_io.sync.data_fence"}, {"storage_io.sync.superblock_fence"},
        {"storage_io.sync.data_fence", true}, {"storage_io.sync.superblock_fence", true},
        {"native_graph.write.metadata.publication", true}
    });
    struct Sample { int64_t value = 0; bool seen = false; };
    static void receive(void *owner, std::string_view key, int64_t value, bool duration) noexcept {
        auto &self = *static_cast<LogicalProfileObservation *>(owner);
        for (size_t i = 0; i < metrics_.size(); ++i) {
            const auto &metric = metrics_[i];
            if (metric.key != key || metric.duration != duration) continue;
            auto &sample = self.samples_[i];
            if (metric.peak) sample.value = sample.seen ? (std::max)(sample.value, value) : value;
            else {
                const auto max = (std::numeric_limits<int64_t>::max)();
                const auto min = (std::numeric_limits<int64_t>::min)();
                sample.value = value > 0 && sample.value > max - value ? max :
                    value < 0 && sample.value < min - value ? min : sample.value + value;
            }
            sample.seen = true;
        }
    }
    std::optional<execution::MaterializedByteBudget> budget_;
    execution::MaterializedByteReservation charge_;
    std::array<Sample, metrics_.size()> samples_{};
    std::optional<std::array<int64_t, 3>> estimate_;
    // Last member: stop observation before destroying charged metadata.
    debug::PerfTrace::ScopedObserver observer_;
public:
    LogicalProfileObservation(const QueryContext &context, size_t maximumBytes)
        : observer_(this, receive) {
        if (context.resource_account) budget_.emplace(maximumBytes, context.resource_account, std::pmr::new_delete_resource());
        else budget_.emplace(maximumBytes);
        charge_ = execution::MaterializedByteReservation(&*budget_);
        charge_.set(stagingBytes());
    }
    void estimate(uint64_t lower, uint64_t expected, uint64_t upper) noexcept {
        if (lower > expected || expected > upper || upper > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return;
        estimate_ = std::array<int64_t,3>{static_cast<int64_t>(lower), static_cast<int64_t>(expected), static_cast<int64_t>(upper)};
    }
    [[nodiscard]] bool hasEstimate() const noexcept { return estimate_.has_value(); }
    [[nodiscard]] PropertyValue estimatedRows() const {
        return estimate_ ? PropertyValue((*estimate_)[1]) : PropertyValue();
    }
    [[nodiscard]] PropertyValue selectedFraction() const {
        const auto rows = value("logical_plan.direct_property.rows");
        return rows > 0 ? PropertyValue(static_cast<double>(rows - value("logical_plan.direct_property.rejected") - value("logical_plan.aggregate.direct_filter_rejected")) / static_cast<double>(rows)) : PropertyValue();
    }
    [[nodiscard]] int64_t value(std::string_view key) const noexcept {
        for (size_t i = 0; i < metrics_.size(); ++i)
            if (!metrics_[i].duration && metrics_[i].key == key) return samples_[i].value;
        return 0;
    }
    [[nodiscard]] std::string_view path() const noexcept {
        if (value("logical_plan.direct_property.batches")) return "direct_property_batches";
        if (value("logical_plan.index_candidate.materialized_bytes")) return "indexed_candidates_with_residual_evaluation";
        if (value("logical_plan.batch.rows")) return "bounded_property_row_adapter";
        return "other_or_no_property_input";
    }
    [[nodiscard]] static size_t stagingBytes() {
        using namespace utils::detail;
        size_t bytes = addStandardHeapBytes(sizeof(LogicalProfileObservation),
            standardFreshUnorderedMapHeapUpper<std::string, PropertyValue>(metrics_.size()));
        for (const auto &metric : metrics_)
            bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(metric.key.size() + 3U));
        bytes = addStandardHeapBytes(bytes, standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(10U));
        for (const auto key : {"phase", "total_time_ms", "calls", "actual_output_rows",
                "access_path", "physical_work", "physical_work_scope", "estimates_available", "estimated_root_rows", "direct_selected_fraction"})
            bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(std::string_view(key).size()));
        bytes = addStandardHeapBytes(bytes, 3U * standardFreshStringHeapUpper(256U));
        return addStandardHeapBytes(bytes, 64U * standardContainerProxyBytes());
    }
    void publishColumns(QueryResult &result) {
        // Keep original columns/order, append discoverable PROFILE fields.
        // Borrow names until setColumnViews has built its admitted owning copy.
        std::pmr::vector<std::string_view> columns(&*budget_);
        columns.reserve(result.getColumns().size() + 10U);
        for (const auto &name : result.getColumns()) columns.push_back(name);
        for (const auto name : {"phase", "total_time_ms", "calls", "actual_output_rows",
                "access_path", "physical_work", "physical_work_scope", "estimates_available",
                "estimated_root_rows", "direct_selected_fraction"})
            if (std::find(columns.begin(), columns.end(), name) == columns.end()) columns.push_back(name);
        result.setColumnViews(columns);
    }
    [[nodiscard]] PropertyValue report() {
        // Full fixed-registry staging was admitted before execution. Result
        // ingestion owns its retained charge before this staging is destroyed.
        PropertyValue::MapType values;
        values.reserve(metrics_.size());
        for (size_t i = 0; i < metrics_.size(); ++i) {
            if (!samples_[i].seen) continue;
            std::string name(metrics_[i].key);
            if (metrics_[i].duration) name += ".ns";
            values.emplace(std::move(name), PropertyValue(samples_[i].value));
        }
        return PropertyValue(std::move(values));
    }
};
} // namespace graph::query::detail
