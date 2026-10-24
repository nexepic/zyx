#include "graph/storage/LogicalGraphNodeScan.hpp"
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
    ASSERT_TRUE(trace.contains("native_graph.read.certified_scan_batches"));
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

class RelocatablePropertyRunTest : public RelocatableStorageBackendTest, public ::testing::WithParamInterface<size_t> {};
TEST_P(RelocatablePropertyRunTest, NativeRunsPreservePageBorrowLifetimeOldPinsAndCopiedFallback) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE; options.buffer_pool_bytes = 64U * 1024U * 1024U;
    RelocatableStorageBackend writer(options); writer.open();
    std::vector<LogicalGraphNodeCreateRequest> inputs; std::vector<std::string> expected;
    for (size_t i = 0U; i < 256U; ++i) {
        expected.push_back(i % 11U == 0U ? std::string{} : i % 7U == 0U ? std::string("a\0b", 3U) : std::string(12U, char('a' + i % 20U)));
        inputs.push_back({{"RunRead"}, {{"text", PropertyValue(expected.back())}, {"rank", PropertyValue(int64_t(i))}}});
    }
    const auto ids = writer.createLogicalNodes(inputs); ASSERT_EQ(ids.size(), expected.size());
    options.buffer_pool_bytes = GetParam();
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    const std::array<std::string_view, 1U> textKey{"text"}, rankKey{"rank"};
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U);
    const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
    struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); } } restore{enabled};
    graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
    {
        graph::query::execution::detail::DirectPropertyColumns columns(*old, ids.size(), 1U, budget);
        columns.readGather(ids, textKey, [](size_t) {}, [] {});
        for (size_t i = 0U; i < ids.size(); ++i)
            EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(i)).data), expected[i]);
    }
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    if (GetParam() > RSS_PAGE_SIZE) {
        ASSERT_TRUE(counters.contains("native_graph.read.property_text_runs"));
        EXPECT_LT(counters.at("native_graph.read.property_text_runs").totalValue, ids.size() / 2U);
        EXPECT_EQ(counters.at("native_graph.read.property_run_cells").totalValue, ids.size());
    }
    {
        graph::query::execution::detail::DirectPropertyColumns columns(*old, ids.size(), 1U, budget);
        columns.readGather(ids, rankKey, [](size_t) {}, [] {});
        for (size_t i = 0U; i < ids.size(); ++i)
            EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(i)).data), int64_t(i));
    }
    std::vector<std::array<std::string, 2U>> aliases(ids.size());
    graph::storage::PropertyColumnBatchReadSinks aliasSinks;
    aliasSinks.scalars = [](std::span<const size_t>, size_t, graph::PropertyType, std::span<const uint64_t>) {
        ADD_FAILURE() << "unexpected scalar in text aliases";
    };
    aliasSinks.texts = [&](std::span<const size_t> rows, size_t column, std::span<const std::string_view> values) {
        ASSERT_EQ(rows.size(), values.size()); ASSERT_LT(column, 2U);
        for (size_t i = 0U; i < rows.size(); ++i) aliases.at(rows[i])[column] = values[i];
    };
    const std::array<std::string_view, 2U> aliasKeys{"text", "text"};
    ASSERT_TRUE(old->tryReadNodePropertyColumnBatchesSortedWithAdmission(ids, aliasKeys,
        [](size_t, size_t, std::optional<PropertyValue>) { ADD_FAILURE() << "unexpected owning alias"; },
        aliasSinks, [](size_t) {}));
    for (size_t i = 0U; i < ids.size(); ++i) {
        EXPECT_EQ(aliases[i][0], expected[i]); EXPECT_EQ(aliases[i][1], expected[i]);
    }
    const auto copied = graph::debug::PerfTrace::snapshotAndReset();
    if (GetParam() > RSS_PAGE_SIZE) {
        ASSERT_TRUE(copied.contains("native_graph.read.property_selected_slot_copy_bytes"));
        EXPECT_GT(copied.at("native_graph.read.property_selected_slot_copy_bytes").totalValue, 0);
    }
    graph::debug::PerfTrace::reset();
    // Exercise the full consumer with the same bounded source cache.
    {
        auto borrowedBackend = std::shared_ptr<RelocatableStorageBackend>(&backend, [](RelocatableStorageBackend *) {});
        graph::query::QueryEngine engine(borrowedBackend);
        auto result = engine.execute("MATCH (n:RunRead) WHERE n.text='aaaaaaaaaaaa' RETURN count(n) AS rows");
        ASSERT_EQ(result.rowCount(), 1U);
        const auto count = std::count(expected.begin(), expected.end(), std::string(12U, 'a'));
        EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), PropertyValue(int64_t(count)));
        const auto production = graph::debug::PerfTrace::snapshotAndReset();
        ASSERT_TRUE(production.contains("logical_plan.direct_property.run_batches"));
        EXPECT_GT(production.at("logical_plan.direct_property.run_batches").totalValue, 0);
        if (GetParam() > RSS_PAGE_SIZE) {
            ASSERT_TRUE(production.contains("native_graph.read.property_text_runs"));
            EXPECT_EQ(production.at("native_graph.read.property_run_cells").totalValue, ids.size());
        }
    }
    writer.replaceLogicalNodeProperties({{ids.front(), {{"text", PropertyValue("changed")}, {"rank", PropertyValue(int64_t{-1})}}}});
    auto current = backend.openLogicalGraphReadSnapshot();
    for (const auto *snapshot : {old.get(), current.get()}) {
        graph::query::execution::detail::DirectPropertyColumns columns(*snapshot, ids.size(), 1U, budget);
        columns.readGather(ids, textKey, [](size_t) {}, [] {});
        for (size_t i = 0U; i < ids.size(); ++i)
            EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(i)).data),
                snapshot == current.get() && i == 0U ? std::string("changed") : expected[i]);
    }
    EXPECT_EQ(budget.used(), 0U);
}
INSTANTIATE_TEST_SUITE_P(BufferModes, RelocatablePropertyRunTest, ::testing::Values(size_t{64U * 1024U * 1024U}, size_t{RSS_PAGE_SIZE}));

TEST_F(RelocatableStorageBackendTest, PropertyRunsKeepComplexLargeValueNullAndMissingFallbacks) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const PropertyValue list(std::vector<PropertyValue>{PropertyValue(int64_t{7})});
    const std::string large(9000U, 'L');
    const auto ids = backend.createLogicalNodes({{{}, {{"value", PropertyValue("a")}}},
        {{}, {{"value", list}}}, {{}, {{"value", PropertyValue(large)}}},
        {{}, {{"value", PropertyValue()}}}, {{}, {}}, {{}, {{"value", PropertyValue(int64_t{3})}}}});
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    const std::array<std::string_view, 1U> keys{"value"};
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U);
    {
        graph::query::execution::detail::DirectPropertyColumns columns(*snapshot, ids.size(), 1U, budget);
        columns.readGather(ids, keys, [](size_t) {}, [] {});
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(0U)).data), "a");
        EXPECT_EQ(*std::get<const PropertyValue *>(columns.cell(0U, columns.rowId(1U)).data), list);
        EXPECT_EQ(std::get<std::string_view>(columns.cell(0U, columns.rowId(2U)).data), large);
        EXPECT_EQ(columns.cell(0U, columns.rowId(3U)).state, graph::query::execution::detail::ColumnCellState::CCS_NULL);
        EXPECT_EQ(columns.cell(0U, columns.rowId(4U)).state, graph::query::execution::detail::ColumnCellState::CCS_MISSING);
        EXPECT_EQ(std::get<int64_t>(columns.cell(0U, columns.rowId(5U)).data), 3);
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST_F(RelocatableStorageBackendTest, CertifiedScanBatchesBoundRowsAndValidateSubsetsBeforeDelivery) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphNodeCreateRequest> inputs;
    for (int64_t i = 0; i < 513; ++i)
        inputs.push_back({{"ScanBatch"}, {{"rank", PropertyValue(i)}, {"text", PropertyValue("x")}}});
    const auto created = backend.createLogicalNodes(inputs);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    const auto *provider = dynamic_cast<const LogicalGraphNodeScanProvider *>(snapshot.get());
    ASSERT_NE(provider, nullptr);
    const std::array<std::string_view, 3> keys{"rank", "text", "rank"};
    size_t calls = 0, count = 0, sourceBytes = 0;
    const auto complete = provider->tryVisitNodeScanBatches(1000, 512, {"ScanBatch"}, keys,
        [&](std::span<const uint64_t> ids, size_t examined, const auto &source) {
            ++calls; count += ids.size(); EXPECT_EQ(ids.size(), examined);
            EXPECT_LE(ids.size(), 512U); EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
            size_t delivered = 0;
            LogicalGraphReadSnapshot::PropertyCellSink owning = [&](size_t, size_t, std::optional<PropertyValue>) { ++delivered; };
            PropertyColumnBatchReadSinks runs;
            runs.scalars = [&](std::span<const size_t> rows, size_t column, graph::PropertyType type, std::span<const uint64_t> bits) {
                EXPECT_TRUE(column == 0 || column == 2); EXPECT_EQ(type, graph::PropertyType::INTEGER);
                EXPECT_EQ(rows.size(), bits.size()); delivered += rows.size();
            };
            runs.texts = [&](std::span<const size_t> rows, size_t column, std::span<const std::string_view> texts) {
                EXPECT_EQ(column, 1U); EXPECT_EQ(rows.size(), texts.size()); delivered += rows.size();
            };
            const std::array<uint64_t, 1> foreign{created.back() + 1};
            EXPECT_THROW(source(foreign, owning, runs, [](size_t) {}), std::invalid_argument);
            const std::array<uint64_t, 2> duplicate{ids.front(), ids.front()};
            EXPECT_THROW(source(duplicate, owning, runs, [](size_t) {}), std::invalid_argument);
            EXPECT_EQ(delivered, 0U);
            source(ids, owning, runs, [&](size_t bytes) { sourceBytes += bytes; });
            EXPECT_EQ(delivered, ids.size() * keys.size());
            return true;
        }, [](size_t bytes) { EXPECT_EQ(bytes, 8192U); });
    ASSERT_TRUE(complete.has_value()); EXPECT_TRUE(*complete);
    EXPECT_EQ(count, 513U); EXPECT_EQ(calls, 2U); EXPECT_GT(sourceBytes, 0U);
}

TEST_F(RelocatableStorageBackendTest, CertifiedScanAdmissionAndConsumerStopDoNotReplay) {
    materializeNativeGraphFixture();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    backend.createLogicalNodes({{{"NewScan"}, {{"alpha", PropertyValue(int64_t{9})}}}});
    const std::array<std::string_view, 1> keys{"alpha"};
    const auto *provider = dynamic_cast<const LogicalGraphNodeScanProvider *>(old.get());
    ASSERT_NE(provider, nullptr);
    size_t calls = 0;
    const auto stop = [&](auto, size_t, const auto &) { ++calls; return false; };
    EXPECT_THROW((void)provider->tryVisitNodeScanBatches(100, 1, {}, keys, stop,
        [](size_t) { throw std::runtime_error("admission denied"); }), std::runtime_error);
    EXPECT_EQ(calls, 0U);
    auto result = provider->tryVisitNodeScanBatches(100, 1, {}, keys, stop, [](size_t) {});
    ASSERT_TRUE(result.has_value()); EXPECT_FALSE(*result); EXPECT_EQ(calls, 1U);
    calls = 0;
    EXPECT_THROW((void)provider->tryVisitNodeScanBatches(100, 1, {}, keys,
        [&](auto, size_t, const auto &) -> bool { ++calls; throw std::runtime_error("consumer stopped"); },
        [](size_t) {}), std::runtime_error);
    EXPECT_EQ(calls, 1U);
    size_t oldMatches = 0;
    result = provider->tryVisitNodeScanBatches(100, 512, {"NewScan"}, keys,
        [&](auto ids, size_t, const auto &) { oldMatches += ids.size(); return true; }, [](size_t) {});
    ASSERT_TRUE(result.has_value()); EXPECT_TRUE(*result); EXPECT_EQ(oldMatches, 0U);
    auto current = backend.openLogicalGraphReadSnapshot();
    provider = dynamic_cast<const LogicalGraphNodeScanProvider *>(current.get());
    ASSERT_NE(provider, nullptr);
    size_t newMatches = 0;
    result = provider->tryVisitNodeScanBatches(100, 512, {"NewScan"}, keys,
        [&](auto ids, size_t, const auto &) { newMatches += ids.size(); return true; }, [](size_t) {});
    EXPECT_EQ(newMatches, 1U);
}

TEST_F(RelocatableStorageBackendTest, CertifiedScanKeepsLargeValueFallbackBeyondSourceWorkspace) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const std::string large(1024U * 1024U + 256U, 'q');
    backend->createLogicalNodes({{{"LargeScan"}, {{"text", PropertyValue(large)}}}});
    graph::query::QueryEngine engine(backend);
    const auto result = engine.execute("MATCH (n:LargeScan) RETURN n.text AS text, count(*) AS rows");
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("rows").asPrimitive(), PropertyValue(int64_t{1}));
    EXPECT_EQ(result.getRows().front().at("text").asPrimitive(), PropertyValue(large));
}
