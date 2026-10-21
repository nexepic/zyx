#include <gtest/gtest.h>
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace graph::query {
namespace {
class AnchorPlanningBackend final : public storage::LogicalGraphReadBackend,
    public storage::LogicalGraphStatisticsBackend {
public:
    mutable size_t planning_reads = 0, label_reads = 0, snapshot_opens = 0;
    bool missing_batch = false, unknown_label = false;
    std::unique_ptr<storage::LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
        ++snapshot_opens; throw std::logic_error("planning must not scan the graph");
    }
    std::optional<storage::LogicalGraphCardinalityStatistics> readLogicalGraphCardinalityStatistics() const override {
        return storage::LogicalGraphCardinalityStatistics{7, 11};
    }
    std::optional<uint64_t> estimateLogicalNodeCardinality(std::span<const std::string>) const override {
        ++label_reads; return unknown_label ? std::nullopt : std::optional<uint64_t>(1);
    }
    storage::LogicalGraphPlanningStatistics readLogicalPlanningStatistics(
        std::span<const std::vector<std::string>> labels) const override {
        ++planning_reads;
        if (missing_batch) return {*readLogicalGraphCardinalityStatistics(), {}};
        return storage::LogicalGraphStatisticsBackend::readLogicalPlanningStatistics(labels);
    }
};
const logical::LogicalTraversal *findTraversal(const logical::LogicalOperator &root) {
    if (const auto *traversal = dynamic_cast<const logical::LogicalTraversal *>(&root)) return traversal;
    for (const auto *child : root.getChildren()) if (child) {
        if (const auto *found = findTraversal(*child)) return found;
    }
    return nullptr;
}
}
TEST(QueryEngineAnchorPlanningTest, OpaqueReadEnvelopesDoNotRequestUnusableAnchorStatistics) {
    for (const auto *query : {
        "MATCH (a)-[r]-(b:Rare) RETURN a,b LIMIT 1",
        "MATCH (a)-[r]-(b:Rare) RETURN a,b SKIP 1",
        "MATCH (a)-[r]-(b:Rare) RETURN DISTINCT a,b",
        "MATCH (a)-[r]-(b:Rare) RETURN a.rank,b",
        "MATCH (a)-[r]-(b:Rare) RETURN rand(),a,b"}) {
        SCOPED_TRACE(query);
        auto backend = std::make_shared<AnchorPlanningBackend>(); QueryEngine engine(backend);
        const auto plan = engine.buildPlan(query); ASSERT_TRUE(plan.root);
        const auto *traversal = findTraversal(*plan.root); ASSERT_NE(traversal, nullptr);
        EXPECT_EQ(traversal->getSourceVar(), "a"); EXPECT_EQ(backend->label_reads, 0U);
        EXPECT_EQ(backend->planning_reads, 1U); EXPECT_EQ(backend->snapshot_opens, 0U);
    }
}
TEST(QueryEngineAnchorPlanningTest, TransparentReadsUseOneFrozenBatchAndUnknownCountsKeepTheOriginalAnchor) {
    for (int mode : {0, 1, 2}) {
        auto backend = std::make_shared<AnchorPlanningBackend>();
        backend->missing_batch = mode == 1; backend->unknown_label = mode == 2;
        QueryEngine engine(backend); const auto plan = engine.buildPlan("MATCH (a)-[r]-(b:Rare) RETURN a,b");
        const auto *traversal = findTraversal(*plan.root); ASSERT_NE(traversal, nullptr);
        EXPECT_EQ(traversal->getSourceVar(), mode ? "a" : "b");
        EXPECT_EQ(backend->label_reads, mode == 1 ? 0U : 1U);
        EXPECT_EQ(backend->planning_reads, 1U); EXPECT_EQ(backend->snapshot_opens, 0U);
    }
}
} // namespace graph::query
