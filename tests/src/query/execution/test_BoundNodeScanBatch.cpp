#include <gtest/gtest.h>
#include "query/execution/BoundNodeScanBatch.hpp"
#include "query/execution/BoundNodeColumnPredicates.hpp"
#include "query/execution/DirectPropertyColumns.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

namespace graph::query::execution::detail {
namespace {
class ScanBatchSnapshot : public storage::LogicalGraphReadSnapshot {
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

TEST(DirectPropertyColumnsTest, ReceivesMixedCellsWithoutAValueMatrixAndKeepsPresence) {
	ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U);
	{
		DirectPropertyColumns columns(snapshot, 6U, 2U, budget);
		EXPECT_THROW(static_cast<void>(columns.rowId(0)), std::out_of_range);
		columns.append(0, 0, PropertyValue(7), [] {});
		columns.append(1, 0, PropertyValue(2.5), [] {});
		columns.append(2, 0, PropertyValue(true), [] {});
		columns.append(3, 0, PropertyValue(std::string(1000, 'x')), [] {});
		columns.append(4, 0, PropertyValue(), [] {});
		columns.append(5, 1, PropertyValue(std::vector<PropertyValue>{PropertyValue(8)}), [] {});
		columns.seal();
		EXPECT_EQ(std::get<int64_t>(columns.cell(0, columns.rowId(0)).data), 7);
		EXPECT_EQ(std::get<double>(columns.cell(0, columns.rowId(1)).data), 2.5);
		EXPECT_TRUE(std::get<bool>(columns.cell(0, columns.rowId(2)).data));
		EXPECT_EQ(std::get<std::string_view>(columns.cell(0, columns.rowId(3)).data), std::string(1000, 'x'));
		EXPECT_EQ(columns.cell(0, columns.rowId(4)).state, ColumnCellState::CCS_NULL);
		EXPECT_EQ(columns.cell(0, columns.rowId(5)).state, ColumnCellState::CCS_MISSING);
		EXPECT_EQ(columns.copyValue(1, columns.rowId(5), [](size_t) {}, [] {}),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(8)}));
		EXPECT_GT(budget.used(), 1000U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(DirectPropertyColumnsTest, RejectsDuplicateCellsForeignRowsAndChangedSnapshots) {
	ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536U);
	DirectPropertyColumns first(snapshot, 1, 1, budget), second(snapshot, 1, 1, budget);
	first.append(0, 0, PropertyValue(1), [] {});
	{
		DirectPropertyColumns malformed(snapshot, 1, 1, budget);
		malformed.append(0, 0, PropertyValue(1), [] {});
		EXPECT_THROW(malformed.append(0, 0, PropertyValue(2), [] {}), std::invalid_argument);
		EXPECT_THROW(malformed.seal(), std::logic_error);
	}
	first.seal(); second.seal();
	EXPECT_THROW(static_cast<void>(second.cell(0, first.rowId(0))), std::invalid_argument);
	EXPECT_THROW(first.append(0, 0, PropertyValue(3), [] {}), std::invalid_argument);
	const auto old = first.rowId(0); first.invalidate();
	EXPECT_THROW(static_cast<void>(first.cell(0, old)), std::invalid_argument);
	++snapshot.identity->catalog_tokens[0];
	EXPECT_THROW(static_cast<void>(second.cell(0, second.rowId(0))), std::invalid_argument);
}

TEST(DirectPropertyColumnsTest, PressureAndCancellationDiscardPrivateConstructionAndRefund) {
	ScanBatchSnapshot snapshot; MaterializedByteBudget budget(8192U);
	EXPECT_THROW((DirectPropertyColumns(snapshot, UINT32_MAX, 1, budget)), MaterializedByteLimitExceeded);
	EXPECT_EQ(budget.used(), 0U);
	{
		DirectPropertyColumns columns(snapshot, 1, 1, budget);
		size_t calls = 0;
		EXPECT_THROW(columns.append(0, 0, PropertyValue(std::string(1000, 'x')),
			[&] { if (++calls == 3) throw std::runtime_error("cancelled"); }), std::runtime_error);
		EXPECT_THROW(static_cast<void>(columns.rowId(0)), std::out_of_range);
		EXPECT_THROW(columns.seal(), std::logic_error);
	}
	EXPECT_EQ(budget.used(), 0U);
}
TEST(DirectPropertyColumnsTest, StringAndMixedTypePredicatesMatchLegacyScalarOrdering) {
    ScanBatchSnapshot snapshot;
    const std::vector<PropertyValue> values{PropertyValue("CN"), PropertyValue("US"), PropertyValue(std::string("a\0b", 3)),
        PropertyValue(""), PropertyValue(3), PropertyValue(3.5), PropertyValue(true), PropertyValue()};
    const std::vector<std::string> names{"value"};
    const std::vector<std::vector<PropertyValue>> legacy{values};
    for (const auto &bound : {PropertyValue("CN"), PropertyValue(3), PropertyValue(3.5), PropertyValue()}) {
        for (int mode = 0; mode < 5; ++mode) {
            MaterializedByteBudget budget(65536U);
            DirectPropertyColumns direct(snapshot, values.size(), 1, budget);
            for (size_t row = 0; row < values.size(); ++row) direct.append(row, 0, values[row], [] {});
            direct.seal();
            logical::LogicalNodeScan scan("n");
            if (mode == 0) scan.setPropertyPredicates({{"value", bound}});
            else scan.setRangePredicates({{"value", mode < 3 ? bound : PropertyValue(), mode < 3 ? PropertyValue() : bound,
                mode == 1, mode == 3}});
            BoundNodeColumnPredicates predicates(scan, {}, names, budget);
            SelectionVector actual(values.size()), expected(values.size());
            (void)predicates.filter(direct, actual, [] {});
            (void)predicates.filter(legacy, expected, budget, [] {});
            for (size_t row = 0; row < values.size(); ++row) EXPECT_EQ(actual.includes(row), expected.includes(row));
        }
    }
}

TEST(DirectPropertyColumnsTest, RawScalarsPreserveIeeeBitsNullAndBudgetWithoutValueDecode) {
    ScanBatchSnapshot snapshot; MaterializedByteBudget budget(65536);
    {
        DirectPropertyColumns columns(snapshot, 6, 1, budget);
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) {
            throw std::logic_error("raw scalar must not allocate a PropertyValue");
        });
        constexpr uint64_t nan = UINT64_C(0x7ff8000000000042), negativeZero = UINT64_C(0x8000000000000000);
        columns.appendScalar(0, 0, PropertyType::INTEGER, std::bit_cast<uint64_t>(int64_t(-7)), [] {});
        columns.appendScalar(1, 0, PropertyType::DOUBLE, nan, [] {});
        columns.appendScalar(2, 0, PropertyType::DOUBLE, negativeZero, [] {});
        columns.appendScalar(3, 0, PropertyType::BOOLEAN, 1, [] {});
        columns.appendScalar(4, 0, PropertyType::NULL_TYPE, 0, [] {});
        columns.seal();
        EXPECT_EQ(std::get<int64_t>(columns.cell(0, columns.rowId(0)).data), -7);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(columns.cell(0, columns.rowId(1)).data)), nan);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(columns.cell(0, columns.rowId(2)).data)), negativeZero);
        EXPECT_TRUE(std::get<bool>(columns.cell(0, columns.rowId(3)).data));
        EXPECT_EQ(columns.cell(0, columns.rowId(4)).state, ColumnCellState::CCS_NULL);
        EXPECT_EQ(columns.cell(0, columns.rowId(5)).state, ColumnCellState::CCS_MISSING);
    }
    EXPECT_EQ(budget.used(), 0U);
    {
        DirectPropertyColumns denied(snapshot, 8, 1, budget);
        MaterializedByteReservation hold(&budget); hold.set(budget.available());
        EXPECT_THROW(denied.appendScalar(0, 0, PropertyType::INTEGER, 1, [] {}), MaterializedByteLimitExceeded);
        EXPECT_THROW(denied.seal(), std::logic_error);
    }
    EXPECT_EQ(budget.used(), 0U);
    for (const auto type : {PropertyType::BOOLEAN, PropertyType::NULL_TYPE, PropertyType::STRING}) {
        DirectPropertyColumns invalid(snapshot, 1, 1, budget);
        EXPECT_THROW(invalid.appendScalar(0, 0, type, 2, [] {}), std::invalid_argument);
        EXPECT_THROW(invalid.seal(), std::logic_error);
    }
}

TEST(DirectPropertyColumnsTest, ScalarGatherRestoresDuplicateIdsAndUsesOwningFallbackForOtherReaders) {
    class ScalarSnapshot final : public ScanBatchSnapshot {
    public:
        bool raw = true;
        mutable size_t oldCalls = 0, rawCalls = 0;
        void readNodePropertyCellsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view>, const PropertyCellSink &sink,
            const std::function<void(size_t)> &admit) const override {
            ++oldCalls;
            for (size_t i = 0; i < ids.size(); ++i) { admit(18); sink(i, 0, PropertyValue(int64_t(ids[i]))); }
        }
        void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view> keys, const PropertyCellSink &sink,
            const storage::PropertyScalarReadSink &scalars, const std::function<void(size_t)> &admit) const override {
            if (!raw) { storage::LogicalGraphReadSnapshot::readNodePropertyCellsWithScalarsSortedWithAdmission(
                ids, keys, sink, scalars, admit); return; }
            ++rawCalls;
            for (size_t i = 0; i < ids.size(); ++i) { admit(18); scalars(i, 0, PropertyType::INTEGER, ids[i]); }
        }
    } snapshot;
    const std::array<uint64_t, 4> ids{4, 2, 4, 1};
    const std::array<std::string_view, 1> keys{"rank"};
    MaterializedByteBudget budget(65536);
    for (bool raw : {true, false}) {
        snapshot.raw = raw;
        DirectPropertyColumns columns(snapshot, ids.size(), 1, budget);
        columns.readGather(ids, keys, [](size_t) {}, [] {});
        for (size_t i = 0; i < ids.size(); ++i)
            EXPECT_EQ(std::get<int64_t>(columns.cell(0, columns.rowId(i)).data), int64_t(ids[i]));
    }
    EXPECT_EQ(snapshot.rawCalls, 1U); EXPECT_EQ(snapshot.oldCalls, 1U); EXPECT_EQ(budget.used(), 0U);
}

}
