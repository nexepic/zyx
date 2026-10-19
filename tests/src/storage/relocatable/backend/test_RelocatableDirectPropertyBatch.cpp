#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "query/execution/DirectPropertyColumns.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <set>

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, DirectPropertyCellsPreserveAliasesPresenceAndPinnedSnapshots) {
    const auto fixture = materializeNativeGraphFixture();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    ASSERT_TRUE(old->supportsNodePropertyCellBatches());
    const auto added = backend.createLogicalNodes({{{}, {{"alpha", PropertyValue()}}}});
    const std::vector<uint64_t> ids{fixture.person_node_id, fixture.unlabeled_node_id, added.front()};
    const std::array<std::string_view, 4> keys{"alpha", "beta", "alpha", "unknown"};
    using Cells = std::map<std::pair<size_t, size_t>, std::optional<PropertyValue>>;
    const auto read = [&](LogicalGraphReadSnapshot &snapshot) {
        Cells result;
        snapshot.readNodePropertyCellsSortedWithAdmission(ids, keys,
            [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                EXPECT_TRUE(result.emplace(std::pair{row, column}, std::move(value)).second);
            }, [](size_t) {});
        return result;
    };
    auto current = backend.openLogicalGraphReadSnapshot();
    const auto before = read(*old), after = read(*current);
    EXPECT_EQ(before.at({0, 0}), PropertyValue(int64_t{42}));
    EXPECT_EQ(before.at({0, 1}), PropertyValue("value"));
    EXPECT_EQ(before.at({0, 0}), before.at({0, 2}));
    EXPECT_FALSE(before.contains({2, 0}));
    ASSERT_TRUE(after.at({2, 0}).has_value());
    EXPECT_EQ(after.at({2, 0})->getType(), graph::PropertyType::NULL_TYPE);
    EXPECT_FALSE(after.contains({0, 3}));
    graph::query::execution::MaterializedByteBudget budget(65536U);
    {
        const std::array<uint64_t, 4> gatherIds{ids[1], ids[0], ids[0], ids[2]};
        graph::query::execution::detail::DirectPropertyColumns gather(*current, gatherIds.size(), keys.size(), budget);
        gather.readGather(gatherIds, keys, [](size_t) {}, [] {});
        EXPECT_EQ(gather.cell(0, gather.rowId(0)).state, graph::query::execution::detail::ColumnCellState::CCS_MISSING);
        EXPECT_EQ(std::get<int64_t>(gather.cell(0, gather.rowId(1)).data), 42);
        EXPECT_EQ(std::get<int64_t>(gather.cell(0, gather.rowId(2)).data), 42);
        EXPECT_EQ(gather.cell(0, gather.rowId(3)).state, graph::query::execution::detail::ColumnCellState::CCS_NULL);
    }
    EXPECT_EQ(budget.used(), 0U);

    const std::array<uint64_t, 2> duplicates{ids[0], ids[0]}, reversed{ids[1], ids[0]};
    size_t calls = 0;
    const LogicalGraphReadSnapshot::PropertyCellSink sink = [&](size_t, size_t, std::optional<PropertyValue>) { ++calls; };
    EXPECT_THROW(current->readNodePropertyCellsSortedWithAdmission(duplicates, keys, sink, [](size_t) {}), std::exception);
    EXPECT_THROW(current->readNodePropertyCellsSortedWithAdmission(reversed, keys, sink, [](size_t) {}), std::exception);
    EXPECT_EQ(calls, 0U);
    EXPECT_THROW(current->readNodePropertyCellsSortedWithAdmission(ids, keys,
        [](size_t, size_t, std::optional<PropertyValue>) { throw std::runtime_error("consumer stopped"); }, [](size_t) {}), std::runtime_error);
}

TEST_F(RelocatableStorageBackendTest, NativeDirectFilterGroupAndDistinctReturnExactOwnedResultsAcrossBatches) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    std::vector<LogicalGraphNodeCreateRequest> inputs;
    struct Expected { int64_t rows = 0, sum = 0; std::set<int64_t> distinct; };
    std::map<std::pair<std::string, bool>, Expected> expected;
    for (int64_t i = 0; i < 600; ++i) {
        const std::string country = i % 2 ? "US" : "CN"; const bool active = i % 3 == 0; const int64_t rank = i % 7;
        inputs.push_back({{"DirectBatch"}, {{"country", PropertyValue(country)}, {"active", PropertyValue(active)}, {"rank", PropertyValue(rank)}}});
        if (country == "CN" && rank >= 2) {
            auto &entry = expected[{country, active}]; ++entry.rows; entry.sum += rank; entry.distinct.insert(rank);
        }
    }
    EXPECT_EQ(backend->createLogicalNodes(inputs).size(), 600U);
    const bool previous = graph::debug::PerfTrace::areValuesEnabled();
    graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
    graph::query::QueryEngine engine(backend);
    auto result = engine.execute("MATCH (n:DirectBatch) WHERE n.country = 'CN' AND n.rank >= 2 RETURN n.country AS country, n.active AS active, count(*) AS rows, count(DISTINCT n.rank) AS distincts, sum(n.rank) AS total");
    const auto trace = graph::debug::PerfTrace::snapshotAndReset(); graph::debug::PerfTrace::setValueCollectionEnabled(previous);
    ASSERT_EQ(result.rowCount(), expected.size());
    int64_t totalRows = 0;
    for (const auto &row : result.getRows()) {
        const auto &country = std::get<std::string>(row.at("country").asPrimitive().getVariant());
        const auto active = std::get<bool>(row.at("active").asPrimitive().getVariant());
        const auto &entry = expected.at({country, active}); totalRows += entry.rows;
        EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(entry.rows));
        EXPECT_EQ(row.at("distincts").asPrimitive(), PropertyValue(static_cast<int64_t>(entry.distinct.size())));
        EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(entry.sum));
    }
    ASSERT_TRUE(trace.contains("native_graph.read.direct_property_batches"));
    EXPECT_GE(trace.at("native_graph.read.direct_property_batches").totalValue, 2);
    ASSERT_TRUE(trace.contains("logical_plan.aggregate.direct_input_rows"));
    EXPECT_EQ(trace.at("logical_plan.aggregate.direct_input_rows").totalValue, totalRows);
    EXPECT_FALSE(trace.contains("logical_plan.batch.rows"));

    // Indexed fallback rows must still pass the retained residual Filter;
    // forwarding a batch receiver must never skip it for index candidates.
    (void)backend->createLogicalNodeIndex({"direct_rank", "DirectBatch", {"rank"}});
    auto indexed = engine.execute("MATCH (n:DirectBatch) WHERE n.rank = 2 RETURN n.country AS country,count(*) AS rows");
    ASSERT_EQ(indexed.rowCount(), 2U);
    for (const auto &row : indexed.getRows()) EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(43));
    backend->close();
    // Result strings/scalars must own data after the snapshot and its batch die.
    EXPECT_EQ(result.rowCount(), 2U);
    EXPECT_EQ(result.getRows()[0].at("country").asPrimitive(), PropertyValue("CN"));
}
