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

    graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
    auto counts = engine.execute("MATCH (n:DirectBatch) RETURN n.country AS country,count(*) AS rows");
    const auto partialTrace = graph::debug::PerfTrace::snapshotAndReset(); graph::debug::PerfTrace::setValueCollectionEnabled(previous);
    ASSERT_EQ(counts.rowCount(), 2U);
    for (const auto &row : counts.getRows()) EXPECT_EQ(row.at("rows").asPrimitive(), PropertyValue(300));
    ASSERT_TRUE(partialTrace.contains("logical_plan.aggregate.partial_count_batches"));
    EXPECT_LT(partialTrace.at("logical_plan.aggregate.partial_count_updates").totalValue, 600);

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

TEST_F(RelocatableStorageBackendTest, DirectPropertyCellsIsolateOldPinsAcrossAnotherHandlePublicationAndFileGrowth) {
    const auto fixture = materializeNativeGraphFixture();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend reader(options), writer(options); reader.open(); writer.open();
    auto old = reader.openLogicalGraphReadSnapshot();
    const std::array<std::string_view, 2> keys{"alpha", "rank"};
    const auto read = [&](LogicalGraphReadSnapshot &snapshot, const std::vector<uint64_t> &ids) {
        std::map<std::pair<size_t, size_t>, PropertyValue> result;
        snapshot.readNodePropertyCellsSortedWithAdmission(ids, keys,
            [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                if (value) EXPECT_TRUE(result.emplace(std::pair{row,column}, std::move(*value)).second);
            }, [](size_t) {});
        return result;
    };
    EXPECT_EQ(read(*old, {fixture.person_node_id}).at({0,0}), PropertyValue(42));
    const auto bytes = std::filesystem::file_size(data_path_);
    std::vector<LogicalGraphNodeCreateRequest> requests;
    for (int64_t i = 0; i < 64; ++i) {
        std::string body(65536U, 'x'); body.replace(0, 8, std::to_string(i));
        requests.push_back({{"Growing"}, {{"rank", PropertyValue(i)}, {"body", PropertyValue(body)}}});
    }
    std::vector<uint64_t> created;
    ASSERT_NO_THROW(created = writer.createLogicalNodes(requests));
    ASSERT_EQ(created.size(), 64U);
    EXPECT_GT(std::filesystem::file_size(data_path_), bytes);
    const std::vector<uint64_t> gather{fixture.person_node_id, created.back()};
    std::map<std::pair<size_t,size_t>, PropertyValue> oldValues;
    ASSERT_NO_THROW(oldValues = read(*old, gather));
    EXPECT_EQ(oldValues.at({0,0}), PropertyValue(42));
    EXPECT_FALSE(oldValues.contains({1,1}));
    std::unique_ptr<LogicalGraphReadSnapshot> current;
    ASSERT_NO_THROW(current = reader.openLogicalGraphReadSnapshot());
    std::map<std::pair<size_t,size_t>, PropertyValue> currentValues;
    ASSERT_NO_THROW(currentValues = read(*current, gather));
    EXPECT_EQ(currentValues.at({0,0}), PropertyValue(42));
    EXPECT_EQ(currentValues.at({1,1}), PropertyValue(63));
    current.reset(); old.reset(); writer.close(); reader.close();
    RelocatableStorageBackend reopened(options); reopened.open();
    auto fresh = reopened.openLogicalGraphReadSnapshot();
    EXPECT_EQ(read(*fresh, gather), currentValues);
}

TEST_F(RelocatableStorageBackendTest, NativeBorrowedViewsUseTypedSinksAndPreserveAliasesAndFallbacks) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Typed"}, {{"integer", PropertyValue(int64_t(-42))},
        {"text", PropertyValue(std::string("a\0b", 3))}, {"null", PropertyValue()},
        {"list", PropertyValue(std::vector<PropertyValue>{PropertyValue(7)})}}}});
    auto old = backend.openLogicalGraphReadSnapshot(); ASSERT_TRUE(old->hasImmutableReadIdentity());
    const std::array<std::string_view, 6> keys{"text", "integer", "text", "null", "list", "missing"};
    std::map<size_t, std::string> texts; std::map<size_t, std::pair<graph::PropertyType, uint64_t>> scalars;
    size_t owning = 0;
    old->readNodePropertyCellsWithViewsSortedWithAdmission(ids, keys,
        [&](size_t row, size_t column, std::optional<PropertyValue> value) {
            EXPECT_EQ(row, 0U); ASSERT_EQ(column, 4U); ASSERT_TRUE(value); ++owning;
            EXPECT_EQ(*value, PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}));
        }, [&](size_t row, size_t column, graph::PropertyType type, uint64_t bits) {
            EXPECT_EQ(row, 0U); EXPECT_TRUE(scalars.emplace(column, std::pair{type, bits}).second);
        }, [&](size_t row, size_t column, std::string_view value) {
            EXPECT_EQ(row, 0U); EXPECT_TRUE(texts.emplace(column, value).second);
        }, [](size_t) {});
    EXPECT_EQ(owning, 1U); ASSERT_EQ(texts.size(), 2U); EXPECT_EQ(texts.at(0), std::string("a\0b", 3));
    EXPECT_EQ(texts.at(0), texts.at(2)); ASSERT_EQ(scalars.size(), 2U);
    EXPECT_EQ(scalars.at(1).second, std::bit_cast<uint64_t>(int64_t(-42)));
    EXPECT_EQ(scalars.at(3).first, graph::PropertyType::NULL_TYPE);
    graph::query::execution::MaterializedByteBudget budget(65536U);
    {
        graph::query::execution::detail::DirectPropertyColumns columns(*old, 1U, keys.size(), budget);
        columns.readGather(ids, keys, [](size_t) {}, [] {});
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0, columns.rowId(0)).data), std::string_view("a\0b", 3));
    }
    EXPECT_EQ(budget.used(), 0U);
}
