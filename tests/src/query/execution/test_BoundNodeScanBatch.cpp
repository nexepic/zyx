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

TEST(DirectPropertyColumnsTest, OrderedGatherUsesLinearWorkAndRestoresSortedDuplicatesWithoutSorting) {
    class ScalarSnapshot final : public ScanBatchSnapshot {
    public:
        mutable size_t owners = 0U;
        void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view>, const PropertyCellSink &,
            const storage::PropertyScalarReadSink &scalars, const std::function<void(size_t)> &) const override {
            owners = ids.size();
            for (size_t i = 0U; i < ids.size(); ++i) {
                if (i && ids[i] <= ids[i - 1U]) throw std::logic_error("source owners are not unique/ordered");
                scalars(i, 0U, PropertyType::INTEGER, ids[i]);
            }
        }
    } snapshot;
    const std::array<std::string_view, 1U> keys{"rank"};
    for (const size_t rows : {64U, 512U, 4096U}) for (const bool duplicates : {false, true}) {
        std::vector<uint64_t> ids;
        for (size_t i = 0U; i < rows; ++i) ids.push_back(1U + (duplicates ? i / 2U : i));
        MaterializedByteBudget budget(4U * 1024U * 1024U);
        size_t checkpoints = 0U;
        {
            DirectPropertyColumns columns(snapshot, rows, 1U, budget);
            columns.readGather(ids, keys, [](size_t) {}, [&] { ++checkpoints; });
            EXPECT_EQ(snapshot.owners, duplicates ? rows / 2U : rows);
            EXPECT_LE(checkpoints, rows * (duplicates ? 4U : 2U));
            for (size_t i = 0U; i < rows; ++i)
                EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(i)).data), int64_t(ids[i]));
            RecordProperty(std::string(duplicates ? "duplicate_" : "unique_") + std::to_string(rows),
                std::to_string(checkpoints));
        }
        EXPECT_EQ(budget.used(), 0U);
    }
}

TEST(DirectPropertyColumnsTest, SynchronousRunsOwnTextAndPreserveScalarBitsNullAndMissing) {
    class RunSnapshot final : public ScanBatchSnapshot {
    public:
        bool hasImmutableReadIdentity() const noexcept override { return true; }
        bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view>, const PropertyCellSink &values,
            const storage::PropertyColumnBatchReadSinks &runs, const std::function<void(size_t)> &) const override {
            if (ids.size() != 10U) throw std::logic_error("unexpected run input");
            const std::array<size_t, 2U> first{0U, 1U}, doubles{2U, 3U};
            std::array<uint64_t, 2U> bits{0U, UINT64_MAX};
            runs.scalars(first, 0U, PropertyType::INTEGER, bits); bits.fill(123U);
            bits = {0x8000000000000000ULL, 0x7ff8000000000042ULL};
            runs.scalars(doubles, 0U, PropertyType::DOUBLE, bits); bits.fill(123U);
            const std::array<size_t, 1U> boolean{4U}, nulls{5U};
            const std::array<uint64_t, 1U> one{1U}, zero{0U};
            runs.scalars(boolean, 0U, PropertyType::BOOLEAN, one);
            runs.scalars(nulls, 0U, PropertyType::NULL_TYPE, zero);
            std::array<std::string, 3U> source{"", std::string("a\0b", 3U), std::string(600U, 'x')};
            const std::array<size_t, 3U> rows{6U, 7U, 8U};
            const std::array<std::string_view, 3U> views{source[0], source[1], source[2]};
            runs.texts(rows, 0U, views);
            for (auto &text : source) text.assign(700U, 'z');
            values(9U, 0U, std::nullopt);
            return true;
        }
    } snapshot;
    const std::array<uint64_t, 10U> ids{1,2,3,4,5,6,7,8,9,10};
    const std::array<std::string_view, 1U> keys{"value"};
    MaterializedByteBudget budget(65536U);
    {
        DirectPropertyColumns columns(snapshot, ids.size(), 1U, budget);
        columns.readGather(ids, keys, [](size_t) {}, [] {});
        EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(0U)).data), 0);
        EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(1U)).data), -1);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(columns.cell(0U, columns.rowId(2U)).data)), 0x8000000000000000ULL);
        EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(columns.cell(0U, columns.rowId(3U)).data)), 0x7ff8000000000042ULL);
        EXPECT_TRUE(std::get<bool>(columns.cell(0U, columns.rowId(4U)).data));
        EXPECT_EQ(columns.cell(0U, columns.rowId(5U)).state, ColumnCellState::CCS_NULL);
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(6U)).data), std::string_view{});
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(7U)).data), std::string_view("a\0b", 3U));
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(8U)).data), std::string(600U, 'x'));
        EXPECT_EQ(columns.cell(0U, columns.rowId(9U)).state, ColumnCellState::CCS_MISSING);
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DirectPropertyColumnsTest, InvalidOrUnsupportedPartialRunsCannotSealAndRefundTheirBudget) {
    class BadSnapshot final : public ScanBatchSnapshot {
    public:
        size_t mode = 0U;
        bool hasImmutableReadIdentity() const noexcept override { return true; }
        bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t>,
            std::span<const std::string_view>, const PropertyCellSink &,
            const storage::PropertyColumnBatchReadSinks &runs, const std::function<void(size_t)> &) const override {
            std::array<size_t, 65U> rows{}; std::array<uint64_t, 65U> bits{};
            rows[0] = 0U; rows[1] = mode == 0U ? 0U : mode == 1U ? 2U : 1U;
            if (mode == 2U) bits[1] = 2U;
            if (mode == 3U) bits[1] = 1U;
            const auto type = mode == 2U ? PropertyType::BOOLEAN : mode == 3U ? PropertyType::NULL_TYPE :
                mode == 4U ? PropertyType::STRING : PropertyType::INTEGER;
            const size_t count = mode == 5U ? 65U : mode == 7U ? 1U : 2U;
            runs.scalars(std::span(rows.data(), count), 0U, type,
                std::span(bits.data(), mode == 6U ? 1U : count));
            return mode != 7U; // False after a valid output is also a failed construction.
        }
    } snapshot;
    const std::array<uint64_t, 2U> ids{1U, 2U}; const std::array<std::string_view, 1U> keys{"value"};
    for (size_t mode = 0U; mode < 8U; ++mode) {
        snapshot.mode = mode; MaterializedByteBudget budget(65536U);
        {
            DirectPropertyColumns columns(snapshot, ids.size(), 1U, budget);
            EXPECT_THROW(columns.readGather(ids, keys, [](size_t) {}, [] {}), std::invalid_argument) << mode;
            EXPECT_THROW(columns.seal(), std::logic_error);
            EXPECT_THROW(static_cast<void>(columns.borrowVersion()), std::logic_error);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
}

TEST(DirectPropertyColumnsTest, ImmutableLegacySourceFallsBackBeforeAnyRunOutput) {
    class LegacySnapshot final : public ScanBatchSnapshot {
    public:
        mutable size_t calls = 0U;
        bool hasImmutableReadIdentity() const noexcept override { return true; }
        void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view>, const PropertyCellSink &,
            const storage::PropertyScalarReadSink &scalars, const std::function<void(size_t)> &) const override {
            ++calls; for (size_t i = 0U; i < ids.size(); ++i) scalars(i, 0U, PropertyType::INTEGER, ids[i]);
        }
    } snapshot;
    const std::array<uint64_t, 2U> ids{3U, 4U}; const std::array<std::string_view, 1U> keys{"value"};
    MaterializedByteBudget budget(65536U);
    {
        DirectPropertyColumns columns(snapshot, ids.size(), 1U, budget);
        columns.readGather(ids, keys, [](size_t) {}, [] {});
        EXPECT_EQ(snapshot.calls, 1U);
        EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(1U)).data), 4);
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(DirectPropertyColumnsTest, RunArenaDenialAndMidCopyCancellationCannotExposePartialColumns) {
    class TextSnapshot final : public ScanBatchSnapshot {
    public:
        bool hasImmutableReadIdentity() const noexcept override { return true; }
        bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t>,
            std::span<const std::string_view>, const PropertyCellSink &,
            const storage::PropertyColumnBatchReadSinks &runs, const std::function<void(size_t)> &) const override {
            const std::string text(600U, 'x');
            const std::array<size_t, 2U> rows{0U, 1U}; const std::array<std::string_view, 2U> views{text, text};
            runs.texts(rows, 0U, views); return true;
        }
    } snapshot;
    const std::array<uint64_t, 2U> ids{1U, 2U}; const std::array<std::string_view, 1U> keys{"text"};
    {
        MaterializedByteBudget budget(1024U);
        {
            DirectPropertyColumns columns(snapshot, ids.size(), 1U, budget);
            EXPECT_THROW(columns.readGather(ids, keys, [](size_t) {}, [] {}), MaterializedByteLimitExceeded);
            EXPECT_THROW(columns.seal(), std::logic_error);
        }
        EXPECT_EQ(budget.used(), 0U);
    }
    MaterializedByteBudget budget(65536U); size_t checks = 0U;
    {
        DirectPropertyColumns columns(snapshot, ids.size(), 1U, budget);
        EXPECT_THROW(columns.readGather(ids, keys, [](size_t) {}, [&] {
            if (++checks == 8U) throw std::runtime_error("copy cancelled");
        }), std::runtime_error);
        EXPECT_THROW(columns.seal(), std::logic_error);
        EXPECT_THROW(static_cast<void>(columns.borrowVersion()), std::logic_error);
    }
    EXPECT_EQ(budget.used(), 0U);
}

}
