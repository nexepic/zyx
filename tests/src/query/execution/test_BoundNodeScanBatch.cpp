#include <gtest/gtest.h>
#include "query/execution/BoundNodeScanBatch.hpp"
#include "query/execution/BoundNodeColumnPredicates.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

namespace graph::query::execution::detail {
namespace {
class ScanBatchSnapshot final : public storage::LogicalGraphReadSnapshot {
public:
	std::optional<storage::LogicalGraphSchemaIdentity> identity{storage::LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 0U}};
	std::optional<storage::LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override { return identity; }
	storage::LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected read"); }
	storage::LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected read"); }
	storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
	storage::LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
		std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override { return {}; }
	void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
		const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
	void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
	void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
};
}

TEST(BoundNodeScanBatchTest, TypedFilterAndEntityColumnsPreserveNullOrderAndDuplicates) {
	logical::LogicalNodeScan scan("n"); scan.setRangePredicates({{"rank", PropertyValue(2), PropertyValue(5), true, false}});
	ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(scan, snapshot, budget, [] {});
	const std::vector<std::string> keys{"rank", "$entity"};
	const std::vector<std::vector<PropertyValue>> columns{{PropertyValue(1), PropertyValue(2), PropertyValue(), PropertyValue(4)},
		{PropertyValue("a"), PropertyValue("b"), PropertyValue("c"), PropertyValue("d")}};
	const std::array<uint64_t, 4U> ids{11U, 12U, 13U, 14U};
	BoundNodeScanBatch batch(program, snapshot, keys, columns, ids, budget, [] {});
	EXPECT_EQ(batch.entity(0U).entity_id, 11U); EXPECT_FALSE(batch.envelope().exactPresence(batch.layout().column(0U)));
	BoundNodeColumnPredicates predicate(scan, {}, keys, budget); SelectionVector selection(4U);
	const auto stats = predicate.filter(batch.envelope(), batch.layout(), selection, [] {});
	EXPECT_EQ(stats.typed_columns, 1U); EXPECT_EQ(selection.selectedCount(), 2U);
	const std::array<size_t, 4U> original{3U, 1U, 1U, 0U}; batch.publishSelection(selection, original, [] {});
	EXPECT_EQ(batch.envelope().selectedCount(), 3U);
	EXPECT_EQ(batch.envelope().originalOrdinal(batch.envelope().selectedRow(0U)), 3U);
	EXPECT_EQ(batch.envelope().selectedRow(1U), batch.envelope().selectedRow(2U));
}

TEST(BoundNodeScanBatchTest, MixedTypeFallbackMatchesOriginalScalarVariantOrdering) {
	logical::LogicalNodeScan scan("n"); scan.setRangePredicates({{"value", PropertyValue(1), PropertyValue(), true, true}});
	ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U); BoundExecutionProgram program(scan, snapshot, budget, [] {});
	const std::vector<std::string> keys{"value"};
	const std::vector<std::vector<PropertyValue>> columns{{PropertyValue(0), PropertyValue(2), PropertyValue(0.5), PropertyValue()}};
	const std::array<uint64_t, 4U> ids{1U, 2U, 3U, 4U};
	BoundNodeScanBatch batch(program, snapshot, keys, columns, ids, budget, [] {});
	BoundNodeColumnPredicates predicates(scan, {}, keys, budget); SelectionVector bound(4U), original(4U);
	const auto stats = predicates.filter(batch.envelope(), batch.layout(), bound, [] {});
	(void) predicates.filter(columns, original, budget, [] {});
	EXPECT_EQ(stats.scalar_columns, 1U);
	for (size_t row = 0U; row < 4U; ++row) EXPECT_EQ(bound.includes(row), original.includes(row));
}

TEST(BoundNodeScanBatchTest, CancellationAtEveryPreparationCheckpointRefundsAllBatchMemory) {
	logical::LogicalNodeScan scan("n"); ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(scan, snapshot, budget, [] {});
	const std::vector<std::string> keys{"value"}; const std::vector<std::vector<PropertyValue>> columns{{PropertyValue(1), PropertyValue(2)}};
	const std::array<uint64_t, 2U> ids{1U, 2U}; size_t checkpoints = 0U;
	const auto baseline = budget.used();
	{ BoundNodeScanBatch batch(program, snapshot, keys, columns, ids, budget, [&] { ++checkpoints; }); }
	ASSERT_GT(checkpoints, 0U); EXPECT_EQ(budget.used(), baseline);
	for (size_t failure = 1U; failure <= checkpoints; ++failure) {
		size_t calls = 0U; EXPECT_THROW((BoundNodeScanBatch(program, snapshot, keys, columns, ids, budget,
			[&] { if (++calls == failure) throw std::runtime_error("cancelled"); })), std::runtime_error);
		EXPECT_EQ(budget.used(), baseline);
	}
}

TEST(BoundNodeScanBatchTest, MalformedSourcesAndChangedSchemaAreRejectedBeforeConsumerEffects) {
	logical::LogicalNodeScan scan("n"); ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U);
	BoundExecutionProgram program(scan, snapshot, budget, [] {});
	const std::vector<std::string> keys{"value"}; const std::vector<std::vector<PropertyValue>> columns{{PropertyValue(1), PropertyValue(2)}};
	const auto baseline = budget.used(); const std::array<uint64_t, 2U> repeated{1U, 1U}, reversed{2U, 1U};
	EXPECT_THROW((BoundNodeScanBatch(program, snapshot, keys, columns, repeated, budget, [] {})), std::invalid_argument);
	EXPECT_THROW((BoundNodeScanBatch(program, snapshot, keys, columns, reversed, budget, [] {})), std::invalid_argument);
	EXPECT_EQ(budget.used(), baseline);
	const std::array<uint64_t, 2U> ids{1U, 2U}; BoundNodeScanBatch batch(program, snapshot, keys, columns, ids, budget, [] {});
	++snapshot.identity->catalog_tokens[0]; EXPECT_THROW(static_cast<void>(batch.entity(0U)), std::runtime_error);
}
}
