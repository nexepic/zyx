#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, UnmodifiedOverlayForwardsCellsAndChangedOverlayFallsBack) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Cell"}, {{"value", PropertyValue(7)}, {"nil", PropertyValue()}}},
        {{"Cell"}, {{"value", PropertyValue(9)}}}});
    const std::array<std::string_view, 4> keys{"value", "nil", "missing", "value"};
    for (bool vector : {false, true}) {
        SCOPED_TRACE(vector);
        auto transaction = backend.beginLogicalGraphTransaction(false);
        LogicalGraphTransactionOverlay overlay(*transaction);
        auto snapshot = vector ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
            : overlay.openLogicalGraphReadSnapshot();
        ASSERT_TRUE(snapshot->supportsNodePropertyCellBatches());
        using Cells = std::map<std::pair<size_t, size_t>, std::optional<PropertyValue>>;
        Cells cells;
        size_t bytes = 0;
        snapshot->readNodePropertyCellsSortedWithAdmission(ids, keys,
            [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                ASSERT_TRUE(cells.emplace(std::pair{row, column}, std::move(value)).second);
            }, [&](size_t size) { bytes += size; });
        EXPECT_GT(bytes, 0U);
        EXPECT_EQ(cells.at({0, 0}), PropertyValue(7));
        EXPECT_EQ(cells.at({1, 3}), PropertyValue(9));
        ASSERT_TRUE(cells.at({0, 1}));
        EXPECT_EQ(cells.at({0, 1})->getType(), graph::PropertyType::NULL_TYPE);
        // Missing can be omitted or explicitly cleared with disengaged optional.
        const auto missing = [&](size_t row, size_t column) {
            const auto found = cells.find({row, column});
            return found == cells.end() || !found->second.has_value();
        };
        EXPECT_TRUE(missing(1, 1));
        EXPECT_TRUE(missing(0, 2));
        EXPECT_TRUE(missing(1, 2));
        const LogicalGraphReadSnapshot::PropertyCellSink ignore = [](size_t, size_t, std::optional<PropertyValue>) {};
        EXPECT_THROW(snapshot->readNodePropertyCellsSortedWithAdmission(ids, keys, ignore,
            [](size_t) { throw std::length_error("admission refused"); }), std::length_error);
        EXPECT_THROW(snapshot->readNodePropertyCellsSortedWithAdmission(ids, keys, {}, [](size_t) {}), std::invalid_argument);
        EXPECT_THROW(snapshot->readNodePropertyCellsWithScalarsSortedWithAdmission(ids, keys, ignore, {}, [](size_t) {}), std::invalid_argument);
        overlay.replaceLogicalNodeProperties({{ids[0], {{"value", PropertyValue(19)}}}});
        EXPECT_FALSE(snapshot->supportsNodePropertyCellBatches());
        EXPECT_THROW(snapshot->readNodePropertyCellsSortedWithAdmission(ids, keys, ignore, [](size_t) {}), std::logic_error);
        EXPECT_EQ(snapshot->readNodeProperty(ids[0], "value"), PropertyValue(19));
    }
}

TEST_F(RelocatableStorageBackendTest, OverlayDirectCellsRejectReentrantMutation) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{"Cell"}, {{"value", PropertyValue(7)}}}});
    const std::array<std::string_view, 1> keys{"value"};
    for (bool vector : {false, true}) for (bool onAdmission : {false, true}) for (bool scalar : {false, true}) {
        SCOPED_TRACE(vector);
        SCOPED_TRACE(onAdmission);
        SCOPED_TRACE(scalar);
        auto transaction = backend.beginLogicalGraphTransaction(false);
        LogicalGraphTransactionOverlay overlay(*transaction);
        auto snapshot = vector ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
            : overlay.openLogicalGraphReadSnapshot();
        ASSERT_TRUE(snapshot->supportsNodePropertyCellBatches());
        const auto mutate = [&] { overlay.replaceLogicalNodeProperties({{ids[0], {{"value", PropertyValue(19)}}}}); };
        const auto admit = [&](size_t) { if (onAdmission) mutate(); };
        const auto sink = [&](size_t, size_t, std::optional<PropertyValue>) { if (!onAdmission) mutate(); };
        const auto scalars = [&](size_t, size_t, graph::PropertyType, uint64_t) { if (!onAdmission) mutate(); };
        if (scalar) EXPECT_THROW(snapshot->readNodePropertyCellsWithScalarsSortedWithAdmission(ids, keys, sink, scalars, admit), std::runtime_error);
        else EXPECT_THROW(snapshot->readNodePropertyCellsSortedWithAdmission(ids, keys, sink, admit), std::runtime_error);
        EXPECT_EQ(snapshot->readNodeProperty(ids[0], "value"), PropertyValue(19));
    }
}

TEST_F(RelocatableStorageBackendTest, PublicTransactionsUseDirectCellsBeforeMutationAndPreserveDirtyReads) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    std::vector<LogicalGraphNodeCreateRequest> nodes;
    for (int i = 0; i < 64; ++i) nodes.push_back({{"Cell"}, {{"value", PropertyValue(i)}}});
    ASSERT_EQ(backend->createLogicalNodes(nodes).size(), 64U);
    backend->close();
    zyx::Database database(data_path_.string()); database.open();
    for (bool readOnly : {false, true}) {
        SCOPED_TRACE(readOnly);
        auto transaction = readOnly ? database.beginReadOnlyTransaction() : database.beginTransaction();
        struct Work { int64_t batches = 0, rows = 0; } work;
        graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(owner);
            if (key == "native_graph.read.direct_property_batches") w.batches += value;
            if (key == "logical_plan.aggregate.direct_input_rows") w.rows += value;
        });
        auto result = transaction.execute("MATCH (n:Cell) WHERE n.value >= 32 RETURN count(n) AS count");
        ASSERT_TRUE(result.isSuccess()) << result.getError(); ASSERT_TRUE(result.hasNext());
        result.next(); EXPECT_EQ(std::get<int64_t>(result.get("count")), 32);
        EXPECT_FALSE(result.hasNext()); EXPECT_GT(work.batches, 0); EXPECT_EQ(work.rows, 32);
        if (!readOnly) {
            auto changed = transaction.execute("MATCH (n:Cell) WHERE n.value=0 SET n.value=99 RETURN n.value AS value");
            ASSERT_TRUE(changed.isSuccess()) << changed.getError(); ASSERT_TRUE(changed.hasNext());
            changed.next(); EXPECT_EQ(std::get<int64_t>(changed.get("value")), 99);
            auto dirty = transaction.execute("MATCH (n:Cell) WHERE n.value >= 32 RETURN count(n) AS count");
            ASSERT_TRUE(dirty.isSuccess()) << dirty.getError(); ASSERT_TRUE(dirty.hasNext());
            dirty.next(); EXPECT_EQ(std::get<int64_t>(dirty.get("count")), 33);
        }
        transaction.rollback();
    }
    database.close();
}
