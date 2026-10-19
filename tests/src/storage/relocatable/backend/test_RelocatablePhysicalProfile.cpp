#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <map>

using namespace relocatable_storage_backend_test;
namespace {
const PropertyValue::MapType &work(const graph::query::QueryResult &result) {
    return std::get<PropertyValue::MapType>(result.getRows().back().at("physical_work").asPrimitive().getVariant());
}
int64_t number(const PropertyValue &value) { return std::get<int64_t>(value.getVariant()); }
void recordWork(const graph::query::QueryResult &result) {
    std::map<std::string, int64_t> ordered;
    for (const auto &[key, value] : work(result)) ordered.emplace(key, number(value));
    std::string json = "{";
    for (const auto &[key, value] : ordered) {
        if (json.size() > 1U) json += ",";
        json += "\"" + key + "\":" + std::to_string(value);
    }
    ::testing::Test::RecordProperty("physical_work_observations", json + "}");
}
}
TEST_F(RelocatableStorageBackendTest, PhysicalProfileReportsDirectWorkWithoutConsumingGlobalTrace) {
    materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
    auto engine = database.getQueryEngine();
    engine->execute("UNWIND range(0,63) AS x CREATE (:PhysicalProfile {rank:x,bucket:x%3,active:x%2=0})");
    graph::debug::PerfTrace::setEnabled(false);
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    struct Reset { ~Reset() { graph::debug::PerfTrace::setEnabled(false); graph::debug::PerfTrace::reset(); } } reset;
    graph::debug::PerfTrace::addValue("sentinel", 123);
    graph::debug::PerfTrace::addValue("logical_plan.direct_property.rows", 12345);
    const auto result = engine->execute("PROFILE MATCH (n:PhysicalProfile) WHERE n.active = true RETURN n.bucket AS bucket,count(DISTINCT n.rank) AS count");
    ASSERT_EQ(result.rowCount(), 4U);
    EXPECT_EQ(number(result.getRows().back().at("actual_output_rows").asPrimitive()), 3);
    EXPECT_EQ(result.getRows().back().at("access_path").asPrimitive(), PropertyValue("direct_property_batches"));
    const auto &metrics = work(result); recordWork(result);
    EXPECT_EQ(number(metrics.at("logical_plan.direct_property.rows")), 64);
    EXPECT_EQ(number(metrics.at("logical_plan.direct_property.rejected")), 0);
    EXPECT_EQ(number(metrics.at("logical_plan.aggregate.direct_filter_rejected")), 32);
    EXPECT_EQ(std::get<double>(result.getRows().back().at("direct_selected_fraction").asPrimitive().getVariant()), 0.5);
    EXPECT_EQ(number(metrics.at("logical_plan.aggregate.direct_input_rows")), 32);
    EXPECT_GT(number(metrics.at("storage.checksum.bytes")), 0);
    EXPECT_GT(number(metrics.at("logical_plan.materialized_budget_peak_bytes")), 0);
    const auto global = graph::debug::PerfTrace::snapshotAndReset();
    EXPECT_EQ(global.at("sentinel").totalValue, 123);
    EXPECT_EQ(global.at("logical_plan.direct_property.rows").totalValue, 12409);
    EXPECT_TRUE(graph::debug::PerfTrace::areValuesEnabled());
    database.close();
    EXPECT_EQ(number(work(result).at("logical_plan.direct_property.rows")), 64);
}
TEST_F(RelocatableStorageBackendTest, PhysicalProfileWritesOnceAndRetainsPublicationPhaseCounts) {
    materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
    auto engine = database.getQueryEngine();
    graph::debug::PerfTrace::setEnabled(false);
    const auto result = engine->execute("PROFILE CREATE (:PhysicalWrite {rank:1}) RETURN 7 AS answer");
    ASSERT_EQ(result.rowCount(), 2U);
    const auto &metrics = work(result); recordWork(result);
    EXPECT_GE(number(metrics.at("native_graph.write.graph_commit_calls")), 1);
    EXPECT_GE(number(metrics.at("native_graph.write.id_reservation_calls")), 1);
    EXPECT_GE(number(metrics.at("storage_io.sync.data_fence")), 1);
    EXPECT_EQ(number(metrics.at("storage_io.sync.data_fence")), number(metrics.at("storage_io.sync.superblock_fence")));
    EXPECT_GT(number(metrics.at("storage_io.completed_write_bytes")), 0);
    EXPECT_EQ(engine->execute("MATCH (n:PhysicalWrite) RETURN count(*) AS count").getRows()[0].at("count").asPrimitive(), PropertyValue(1));
    EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
    database.close();
}
TEST_F(RelocatableStorageBackendTest, PhysicalProfileAdmissionPrecedesMutationAndRefundsBeforeRetry) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    graph::query::LogicalReadExecutionOptions reads; reads.bounds.maximum_materialized_bytes = 512;
    graph::query::QueryEngine small(backend, reads);
    const auto before = backend->currentNativeLogicalGraphMutationFence();
    EXPECT_THROW((void)small.execute("PROFILE CREATE (:MustNotPublish) RETURN 1 AS value"), std::exception);
    EXPECT_EQ(backend->currentNativeLogicalGraphMutationFence(), before);
    EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
    graph::query::QueryEngine retry(backend);
    EXPECT_EQ(retry.execute("PROFILE RETURN 9 AS value").getRows()[0].at("value").asPrimitive(), PropertyValue(9));
    backend->close();
}
TEST_F(RelocatableStorageBackendTest, PhysicalReadonlyProfileAndExplainNeverUseScratchOrPublish) {
    materializeNativeDatabase();
    auto options = backendOptions();
    RelocatableStorageBackend initial(options); initial.open(); initial.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const auto bytes = std::filesystem::file_size(data_path_);
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    backend->resetIoStats(); graph::query::QueryEngine engine(backend);
    const auto explained = engine.execute("EXPLAIN MATCH (n) RETURN n");
    ASSERT_GT(explained.rowCount(), 0U);
    bool contract = false;
    for (const auto &row : explained.getRows())
        contract |= row.at("details").toString().find("runtime selects index/label/full scan") != std::string::npos;
    EXPECT_TRUE(contract);
    const auto result = engine.execute("PROFILE RETURN 4 AS value");
    EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(4));
    EXPECT_FALSE(work(result).contains("storage_io.completed_write_bytes"));
    EXPECT_EQ(backend->ioStats().data.write_bytes, 0U);
    EXPECT_EQ(backend->currentNativeLogicalGraphMutationFence(), fence);
    backend->close(); EXPECT_EQ(std::filesystem::file_size(data_path_), bytes);
}

TEST_F(RelocatableStorageBackendTest, PhysicalProfileCoalescesSharedOwnerUpdatesAndNoOpsKeepOldPins) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    std::vector<LogicalGraphNodeCreateRequest> inputs;
    for (size_t i = 0; i < 8; ++i)
        inputs.push_back({{"Coalesced"}, {{"rank", PropertyValue(0)}, {"body", PropertyValue("unchanged")}}});
    const auto ids = backend->createLogicalNodes(inputs);
    auto old = backend->openLogicalGraphReadSnapshot();
    graph::query::QueryEngine engine(backend, backend);
    const auto result = engine.execute("PROFILE MATCH (n:Coalesced) SET n.rank = n.rank + 1 RETURN n.rank AS rank");
    ASSERT_EQ(result.rowCount(), 9U);
    const auto &metrics = work(result); recordWork(result);
    EXPECT_EQ(number(metrics.at("native_graph.write.property_plan_owners")), 8);
    EXPECT_EQ(number(metrics.at("native_graph.write.property_plan_coalesced_members")), 8);
    EXPECT_EQ(number(metrics.at("native_graph.write.property_plan_replacement_images")), 1);
    EXPECT_FALSE(metrics.contains("native_graph.write.property_pool_catalog_scans"));
    for (size_t i = 0; i < 8; ++i) EXPECT_EQ(result.getRows()[i].at("rank").asPrimitive(), PropertyValue(1));
    for (const auto id : ids) EXPECT_EQ(old->readNodeProperty(id, "rank"), PropertyValue(0));
    const auto noOp = engine.execute("PROFILE MATCH (n:Coalesced) SET n.rank = 1 RETURN n.rank AS rank");
    EXPECT_FALSE(work(noOp).contains("native_graph.write.property_plan_replacement_images"));
    EXPECT_FALSE(work(noOp).contains("native_graph.write.property_pool_catalog_scans"));
    old.reset(); backend->close(); backend->open();
    auto current = backend->openLogicalGraphReadSnapshot();
    for (const auto id : ids) {
        EXPECT_EQ(current->readNodeProperty(id, "rank"), PropertyValue(1));
        EXPECT_EQ(current->readNodeProperty(id, "body"), PropertyValue("unchanged"));
    }
}

TEST_F(RelocatableStorageBackendTest, PhysicalProfileOutputAndFailureRefundTheSameQueryAccount) {
    using namespace graph::query::execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    graph::query::QueryEngine engine(backend);
    auto account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(), ResourceClass::RCK_QUERY, 1024U*1024U);
    graph::query::QueryContext context; context.resource_account = account;
    {
        auto result = engine.execute("PROFILE RETURN 'owned' AS value", context);
        EXPECT_GT(account->used(), 0U);
        backend->close();
        EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue("owned"));
        EXPECT_TRUE(result.getRows().back().contains("physical_work"));
    }
    EXPECT_EQ(account->used(), 0U);
    backend->open();
    // A semantic failure after observation begins must restore thread state and
    // release all query metadata before a subsequent execution.
    EXPECT_THROW((void)engine.execute("PROFILE RETURN 1/0 AS value", context), std::exception);
    EXPECT_EQ(account->used(), 0U);
    EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
    EXPECT_EQ(engine.execute("PROFILE RETURN 5 AS value", context).getRows()[0].at("value").asPrimitive(), PropertyValue(5));
}

TEST_F(RelocatableStorageBackendTest, PhysicalProfileGuardFailureInsideObservationRefundsAndRestoresThreadState) {
    using namespace graph::query::execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    graph::query::QueryEngine engine(backend);
    auto account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(), ResourceClass::RCK_QUERY, 1024U*1024U);
    graph::debug::PerfTrace::setEnabled(false);
    bool sampledInside = false;
    graph::query::QueryGuard guard(0, 1, [&] {
        // Baseline and pre-execution checks see zero. Inject resident pressure
        // only after the per-query observer is active, without timing/sleeps.
        sampledInside |= graph::debug::PerfTrace::areValuesEnabled();
        return sampledInside ? size_t{2U*1024U*1024U} : size_t{0};
    });
    graph::query::QueryContext context; context.resource_account = account; context.guard = &guard;
    const auto fence = backend->currentNativeLogicalGraphMutationFence();
    EXPECT_THROW((void)engine.execute("PROFILE MATCH (n) RETURN count(*) AS count", context),
        graph::query::QueryMemoryExceededException);
    EXPECT_TRUE(sampledInside); EXPECT_EQ(account->used(), 0U);
    EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
    EXPECT_EQ(backend->currentNativeLogicalGraphMutationFence(), fence);
    context.guard = nullptr;
    EXPECT_EQ(engine.execute("PROFILE RETURN 6 AS value", context).getRows()[0].at("value").asPrimitive(), PropertyValue(6));
}
TEST_F(RelocatableStorageBackendTest, PhysicalProfileUsesBoundedExistingEstimatesAndMarksUnsupportedRootsUnavailable) {
    materializeNativeDatabase(); graph::Database database(data_path_.string()); database.open();
    auto engine = database.getQueryEngine(); engine->execute("CREATE (:Estimated {rank:1}),(:Estimated {rank:2})");
    const auto scan = engine->execute("PROFILE MATCH (n:Estimated) RETURN n.rank AS rank");
    ASSERT_EQ(scan.rowCount(), 3U);
    EXPECT_EQ(scan.getRows().back().at("estimates_available").asPrimitive(), PropertyValue(true));
    EXPECT_EQ(scan.getRows().back().at("estimated_root_rows").asPrimitive(), PropertyValue(2));
    const auto aggregate = engine->execute("PROFILE MATCH (n:Estimated) RETURN count(*) AS count");
    EXPECT_EQ(aggregate.getRows().back().at("estimates_available").asPrimitive(), PropertyValue(false));
    EXPECT_EQ(aggregate.getRows().back().at("estimated_root_rows").asPrimitive(), PropertyValue());
}
