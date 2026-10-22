#include <gtest/gtest.h>
#include "query/execution/BatchEnvelope.hpp"

namespace graph::query::execution::detail {
namespace {
class EnvelopeSnapshot final : public storage::LogicalGraphReadSnapshot {
public:
	storage::LogicalGraphSchemaIdentity identity{1U, 2U, {}, 0U, 0U};
	bool tokens = true;
	bool immutable = false;
	mutable size_t identity_reads = 0;
	bool hasImmutableReadIdentity() const noexcept override { return immutable; }
	std::optional<storage::LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override { ++identity_reads; return identity; }
	bool supportsPropertyTokenResolution() const override { return tokens; }
	storage::LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected read"); }
	storage::LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected read"); }
	void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
	void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
	storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
	storage::LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
		std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override { return {}; }
	void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
		const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
};
class EnvelopeAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U, fail_at = SIZE_MAX, live = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (calls++ == fail_at) throw std::bad_alloc();
		auto *result = std::pmr::new_delete_resource()->allocate(bytes, alignment); live += bytes; return result;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment); live -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

}

TEST(BatchEnvelopeTest, PreservesTypedStatesEntityKindNestedOwnersSelectionOrderAndDuplicates) {
	MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot;
	const std::array<std::string, 5U> names{"integer", "double", "boolean", "entity", "dynamic"};
	BoundColumnLayout layout(names, budget);
	FixedWidthBatch<int64_t, double, bool> fixed(3U, budget);
	fixed.append(TypedCell<int64_t>::withValue(11), TypedCell<double>::withValue(1.5), TypedCell<bool>::withValue(true));
	fixed.append(TypedCell<int64_t>::null(), TypedCell<double>::missing(), TypedCell<bool>::withValue(false));
	fixed.append(TypedCell<int64_t>::missing(), TypedCell<double>::null(), TypedCell<bool>::null());
	EntityRefColumn entities(3U, snapshot, budget);
	entities.append(storage::EntityRef{7U, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE, [] {});
	entities.append(storage::EntityRef{7U, true, 1U, 1U, 3U}, ColumnCellState::CCS_VALUE, [] {});
	entities.append(std::nullopt, ColumnCellState::CCS_MISSING, [] {});
	DynamicValueArena arena({128U, 4096U, 128U, 128U, 16U}, budget);
	const std::array<PropertyValue, 2U> values{PropertyValue(PropertyValue::MapType{{"nested", PropertyValue(std::vector<PropertyValue>{PropertyValue("payload"), PropertyValue(2)})}}), PropertyValue()};
	(void) arena.appendMany(values, [] {}); (void) arena.appendMissing();
	const auto arenaVersion = arena.borrowVersion();
	const std::array sources{BatchColumnSource::fixed<0>(fixed), BatchColumnSource::fixed<1>(fixed), BatchColumnSource::fixed<2>(fixed),
		BatchColumnSource::entity(entities), BatchColumnSource::dynamic(arena, {0U, 3U, arenaVersion.owner, arenaVersion.incarnation})};
	BatchEnvelope batch(10U, layout, snapshot, 5U, budget);
	const std::array<uint32_t, 3U> selected{2U, 0U, 0U}; const std::array<uint64_t, 3U> ordinals{100U, 101U, 102U};
	batch.publish(names, sources, 3U, selected, ordinals, [] {});
	EXPECT_EQ(batch.selectedCount(), 3U); EXPECT_EQ(batch.originalOrdinal(batch.selectedRow(0)), 102U);
	EXPECT_EQ(batch.selectedRow(1), batch.selectedRow(2));
	EXPECT_EQ(std::get<int64_t>(batch.cell(layout.column(0), batch.rowId(0)).data), 11);
	EXPECT_EQ(batch.cell(layout.column(0), batch.rowId(1)).state, ColumnCellState::CCS_NULL);
	EXPECT_EQ(batch.cell(layout.column(0), batch.rowId(2)).state, ColumnCellState::CCS_MISSING);
	EXPECT_FALSE(std::get<bool>(batch.cell(layout.column(2), batch.rowId(1)).data));
	EXPECT_FALSE(std::get<storage::EntityRef>(batch.cell(layout.column(3), batch.rowId(0)).data).is_edge);
	EXPECT_TRUE(std::get<storage::EntityRef>(batch.cell(layout.column(3), batch.rowId(1)).data).is_edge);
	const auto nested = std::get<DynamicCellView>(batch.cell(layout.column(4), batch.rowId(0)).data);
	EXPECT_EQ(nested.arena->type(nested.value), PropertyType::MAP);
	EXPECT_EQ(batch.cell(layout.column(4), batch.rowId(2)).state, ColumnCellState::CCS_MISSING);
	EXPECT_TRUE(batch.exactPresence(layout.column(4)));
	const auto row = batch.rowId(0); batch.release(); EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0), row)), std::invalid_argument);
}

TEST(BatchEnvelopeTest, RejectsSourceReuseForeignLayoutsBudgetsSnapshotsAndPrograms) {
	MaterializedByteBudget budget(65536U), otherBudget(65536U); EnvelopeSnapshot snapshot, otherSnapshot;
	const std::array<std::string, 1U> names{"value"}; BoundColumnLayout layout(names, budget), otherLayout(names, budget), foreignBudgetLayout(names, otherBudget);
	FixedWidthBatch<int64_t> source(2U, budget), foreignSource(2U, otherBudget);
	source.append(TypedCell<int64_t>::withValue(3));
	foreignSource.append(TypedCell<int64_t>::withValue(3));
	BatchEnvelope batch(10U, layout, snapshot, 2U, budget);
	const std::array sources{BatchColumnSource::fixed<0>(source)}; const std::array<uint32_t, 1U> selected{0U}; const std::array<uint64_t, 1U> ordinals{9U};
	batch.publish(names, sources, 1U, selected, ordinals, [] {});
	EXPECT_THROW(batch.validateProgram(11U), std::invalid_argument);
	EXPECT_THROW(batch.validateSnapshot(otherSnapshot), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(batch.cell(otherLayout.column(0), batch.rowId(0))), std::invalid_argument);
	const std::array foreign{BatchColumnSource::fixed<0>(foreignSource)};
	EXPECT_THROW(batch.publish(names, foreign, 1U, selected, ordinals, [] {}), std::invalid_argument);
	EXPECT_THROW((BatchEnvelope(10U, foreignBudgetLayout, snapshot, 2U, budget)), std::invalid_argument);
	source.clear(); source.append(TypedCell<int64_t>::withValue(77));
	EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0), batch.rowId(0))), std::invalid_argument);
	EntityRefColumn foreignEntities(1U, otherSnapshot, budget); foreignEntities.append(storage::EntityRef{1U, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE, [] {});
	const std::array entities{BatchColumnSource::entity(foreignEntities)};
	EXPECT_THROW(batch.publish(names, entities, 1U, selected, ordinals, [] {}), std::invalid_argument);
}

TEST(BatchEnvelopeTest, InvalidShapeKindSelectionAndFailedPublicationKeepTheOldCompletePrefix) {
	MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot; const std::array<std::string, 1U> names{"value"}; BoundColumnLayout layout(names, budget);
	FixedWidthBatch<int64_t> source(2U, budget); source.append(TypedCell<int64_t>::withValue(3));
	BatchEnvelope batch(10U, layout, snapshot, 2U, budget); std::array sources{BatchColumnSource::fixed<0>(source)};
	const std::array<uint32_t, 1U> selected{0U}; const std::array<uint64_t, 1U> ordinals{9U};
	batch.publish(names, sources, 1U, selected, ordinals, [] {}); const auto old = batch.rowId(0);
	sources[0].kind = BatchColumnKind::BCK_DOUBLE;
	EXPECT_THROW(batch.publish(names, sources, 1U, selected, ordinals, [] {}), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0), old)), std::invalid_argument);
	EXPECT_EQ(std::get<int64_t>(batch.cell(layout.column(0), batch.rowId(0)).data), 3);
	sources[0] = BatchColumnSource::fixed<0>(source);
	const std::array<uint32_t, 1U> invalid{1U}; EXPECT_THROW(batch.publish(names, sources, 1U, invalid, ordinals, [] {}), std::out_of_range);
	size_t calls = 0U; batch.publish(names, sources, 1U, selected, ordinals, [&] { ++calls; });
	for (size_t stop = 1U; stop <= calls; ++stop) {
		size_t seen = 0U; const auto previousCharge = budget.used();
		EXPECT_THROW(batch.publish(names, sources, 1U, selected, ordinals, [&] {
			EXPECT_EQ(std::get<int64_t>(batch.cell(layout.column(0), batch.rowId(0)).data), 3);
			if (++seen == stop) throw std::runtime_error("cancelled");
		}), std::runtime_error);
		EXPECT_EQ(budget.used(), previousCharge); EXPECT_EQ(batch.size(), 1U);
	}
}

TEST(BatchEnvelopeTest, EntityAppendRollbackAndReaderEpochChangesRejectStaleValues) {
	MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot; EntityRefColumn entities(2U, snapshot, budget);
	entities.append(storage::EntityRef{1U, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE, [] {});
	const auto old = entities.borrowVersion();
	EXPECT_THROW(entities.append(storage::EntityRef{0U, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE, [] {}), std::invalid_argument);
	EXPECT_EQ(entities.size(), 1U); EXPECT_THROW(entities.validateBorrowVersion(old), std::invalid_argument);
	size_t calls = 0U;
	EXPECT_THROW(entities.append(storage::EntityRef{2U, false, 0U, 0U, 0U}, ColumnCellState::CCS_VALUE,
		[&] { if (++calls == 4U) throw std::runtime_error("cancelled"); }), std::runtime_error);
	EXPECT_EQ(entities.size(), 1U); EXPECT_EQ(entities.value(entities.borrowVersion()).entity_id, 1U);
	++snapshot.identity.catalog_tokens[0]; EXPECT_THROW(entities.validateBorrowVersion(entities.borrowVersion()), std::invalid_argument);
	entities.release(); EXPECT_EQ(budget.used(), 0U);
}

TEST(BatchEnvelopeTest, LegacyNumericBridgeExplicitlyDeclinesExactPresence) {
	MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot; const std::array<std::string, 1U> names{"value"}; BoundColumnLayout layout(names, budget);
	const std::array<std::vector<PropertyValue>, 1U> values{std::vector<PropertyValue>{PropertyValue(3), PropertyValue()}};
	PrimitiveNumericColumns numeric(budget); const std::array<size_t, 1U> requested{0U}; ASSERT_TRUE(numeric.tryPrepare(values, requested, [] {}));
	BatchEnvelope batch(10U, layout, snapshot, 2U, budget); const std::array sources{BatchColumnSource::numeric(numeric, 0U)};
	const std::array<uint32_t, 2U> selected{0U, 1U}; const std::array<uint64_t, 2U> ordinals{0U, 1U};
	batch.publish(names, sources, 2U, selected, ordinals, [] {}); EXPECT_FALSE(batch.exactPresence(layout.column(0)));
	EXPECT_EQ(std::get<int64_t>(batch.cell(layout.column(0), batch.rowId(0)).data), 3);
	EXPECT_EQ(batch.cell(layout.column(0), batch.rowId(1)).state, ColumnCellState::CCS_NULL);
	numeric.release(); EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0), batch.rowId(0))), std::invalid_argument);
}
}

namespace graph::query::execution::detail {
TEST(BatchEnvelopeTest, EveryMetadataAllocationFailurePreservesPublishedPrefixAndRefundsAdmission) {
	for (size_t failure = 0U; failure < 3U; ++failure) {
		SCOPED_TRACE(failure);
		MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot; EnvelopeAllocator upstream;
		const std::array<std::string, 1U> names{"value"}; BoundColumnLayout layout(names, budget);
		FixedWidthBatch<int64_t> source(2U, budget); source.append(TypedCell<int64_t>::withValue(3));
		const std::array sources{BatchColumnSource::fixed<0>(source)};
		const std::array<uint32_t, 1U> selected{0U}; const std::array<uint64_t, 1U> ordinals{9U};
		BatchEnvelope batch(1U, layout, snapshot, 2U, budget, &upstream);
		batch.publish(names, sources, 1U, selected, ordinals, [] {});
		const auto old = batch.rowId(0U); const auto charge = budget.used(); const auto live = upstream.live;
		upstream.fail_at = upstream.calls + failure;
		EXPECT_THROW(batch.publish(names, sources, 1U, selected, ordinals, [] {}), std::bad_alloc);
		EXPECT_EQ(budget.used(), charge); EXPECT_EQ(upstream.live, live);
		EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0U), old)), std::invalid_argument);
		EXPECT_EQ(std::get<int64_t>(batch.cell(layout.column(0U), batch.rowId(0U)).data), 3);
		batch.release(); EXPECT_EQ(upstream.live, 0U);
	}
}
TEST(BatchEnvelopeTest, DirectStringsAndPresenceUseTheSharedEnvelopeAndRejectExpiredOwners) {
    MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot;
    const std::array<std::string, 1> names{"value"}; BoundColumnLayout layout(names, budget);
    DirectPropertyColumns owner(snapshot, 3, 1, budget);
    owner.append(0, 0, PropertyValue(std::string("a\0b", 3)), [] {});
    owner.append(1, 0, PropertyValue(), [] {}); owner.seal();
    const std::array sources{BatchColumnSource::direct(owner, 0)};
    const std::array<uint32_t, 3> selection{2, 0, 0}; const std::array<uint64_t, 3> ordinals{9, 8, 7};
    BatchEnvelope batch(1, layout, snapshot, 3, budget);
    batch.publish(names, sources, 3, selection, ordinals, [] {});
    EXPECT_TRUE(batch.exactPresence(layout.column(0)));
    EXPECT_EQ(std::get<std::string_view>(batch.cell(layout.column(0), batch.rowId(0)).data), std::string_view("a\0b", 3));
    EXPECT_EQ(batch.cell(layout.column(0), batch.rowId(1)).state, ColumnCellState::CCS_NULL);
    EXPECT_EQ(batch.cell(layout.column(0), batch.rowId(2)).state, ColumnCellState::CCS_MISSING);
    EXPECT_EQ(batch.selectedRow(1), batch.selectedRow(2));
    owner.invalidate();
    EXPECT_THROW(static_cast<void>(batch.cell(layout.column(0), batch.rowId(0))), std::invalid_argument);
}

}

namespace graph::query::execution::detail {
TEST(BatchEnvelopeTest, DirectBorrowedTextOwnsItsBytesAndImmutableIdentityChecksAtBatchBoundaries) {
    MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot; snapshot.immutable = true;
    {
        DirectPropertyColumns columns(snapshot, 2U, 1U, budget);
        std::string buffer("a\0b", 3); columns.appendText(0, 0, buffer, [] {});
        buffer.assign("changed"); columns.appendText(1, 0, buffer, [] {}); buffer.assign("reused");
        columns.seal(); const size_t checks = snapshot.identity_reads;
        for (size_t i = 0; i < 1000; ++i)
            EXPECT_EQ(std::get<std::string_view>(columns.cell(0, columns.rowId(0)).data), std::string_view("a\0b", 3));
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0, columns.rowId(1)).data), "changed");
        EXPECT_EQ(snapshot.identity_reads, checks); EXPECT_LE(checks, 2U);
        const std::array<std::string, 1> names{"text"}; BoundColumnLayout layout(names, budget);
        BatchEnvelope envelope(1U, layout, snapshot, 2U, budget);
        const std::array sources{BatchColumnSource::direct(columns, 0U)};
        const std::array<uint32_t, 2> selected{0U, 1U}; const std::array<uint64_t, 2> ordinals{0U, 1U};
        envelope.publish(names, sources, 2U, selected, ordinals, [] {});
        const size_t publishedChecks = snapshot.identity_reads;
        for (size_t i = 0; i < 1000; ++i)
            EXPECT_EQ(std::get<std::string_view>(envelope.cell(layout.column(0), envelope.rowId(0)).data), std::string_view("a\0b", 3));
        EXPECT_EQ(snapshot.identity_reads, publishedChecks);

    }
    EXPECT_EQ(budget.used(), 0U);
    snapshot.immutable = false;
    DirectPropertyColumns mutableColumns(snapshot, 1U, 1U, budget);
    mutableColumns.appendText(0, 0, "ok", [] {}); mutableColumns.seal();
    ++snapshot.identity.catalog_tokens[0];
    EXPECT_THROW((void)mutableColumns.cell(0, mutableColumns.rowId(0)), std::invalid_argument);
}
TEST(BatchEnvelopeTest, DirectBorrowedTextFailureDoesNotPublishPartialCellsOrLeakBudget) {
    MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot;
    {
        DirectPropertyColumns columns(snapshot, 1U, 1U, budget); size_t checkpoints = 0;
        EXPECT_THROW(columns.appendText(0, 0, std::string(2048, 'x'), [&] {
            if (++checkpoints == 3U) throw std::runtime_error("cancelled");
        }), std::runtime_error);
        EXPECT_THROW(columns.seal(), std::logic_error);
    }
    EXPECT_EQ(budget.used(), 0U);
    {
        DirectPropertyColumns columns(snapshot, 1U, 1U, budget);
        EXPECT_THROW(columns.appendText(0, 0, std::string(131072, 'x'), [] {}), std::exception);
        EXPECT_THROW(columns.seal(), std::logic_error);
    }
    EXPECT_EQ(budget.used(), 0U);
}
}

namespace graph::query::execution::detail {
TEST(BatchEnvelopeTest, ShortTextKernelPreservesSelectionDuplicatesAbsenceAndDeclinesBeforeEffects) {
    MaterializedByteBudget budget(65536U); EnvelopeSnapshot snapshot;
    const std::array<size_t, 5> order{2, 0, 0, 1, 3}; SelectionVector selected(4); selected.reject(3);
    DirectPropertyColumns columns(snapshot, 4, 1, budget);
    columns.appendText(0, 0, std::string_view("a\0b", 3), [] {});
    columns.appendText(1, 0, "", [] {}); columns.append(2, 0, PropertyValue(), [] {});
    columns.appendScalar(3, 0, PropertyType::INTEGER, 7, [] {}); columns.seal();
    std::vector<std::optional<std::string>> values;
    ASSERT_TRUE(columns.tryVisitShortTextColumn(0, selected, order, [&](size_t, auto text) {
        values.emplace_back(text ? std::optional<std::string>(*text) : std::nullopt);
    }, [] {}));
    ASSERT_EQ(values.size(), 4U); EXPECT_FALSE(values[0]); EXPECT_EQ(values[1], std::string("a\0b", 3));
    EXPECT_EQ(values[1], values[2]); EXPECT_EQ(values[3], std::string{});
    size_t effects = 0; selected.select(3);
    EXPECT_FALSE(columns.tryVisitShortTextColumn(0, selected, order, [&](size_t, auto) { ++effects; }, [] {}));
    EXPECT_EQ(effects, 0U);
    DirectPropertyColumns longText(snapshot, 1, 1, budget); longText.appendText(0, 0, std::string(65, 'x'), [] {}); longText.seal();
    const std::array<size_t, 1> first{0}; SelectionVector one(1);
    EXPECT_FALSE(longText.tryVisitShortTextColumn(0, one, first, [&](size_t, auto) { ++effects; }, [] {}));
    EXPECT_EQ(effects, 0U);
}
}
