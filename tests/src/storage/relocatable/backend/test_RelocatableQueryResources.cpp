#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "query/execution/ProcessResourceDomains.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "query/execution/BoundPropertyBindings.hpp"
#include <chrono>
#include <barrier>
#include <thread>

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, NativeQueryOutputRetainsItsMemoryDomainAfterClose) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
	(void)backend->createLogicalNodes({{{"Memory"}, {{"payload", PropertyValue(std::string(4000U, 'x'))}}}});
	std::weak_ptr<execution::detail::DatabaseResourceBudget> domain = backend->queryResourceDomain();
	query::QueryEngine engine(backend);
	auto result = engine.execute("MATCH (n:Memory) RETURN n");
	ASSERT_EQ(result.rowCount(), 1U); ASSERT_GT(result.retainedResourceBytes(), 4000U);
	EXPECT_EQ(backend->queryResourceDomain()->usage().classes[static_cast<size_t>(execution::detail::ResourceClass::RCK_QUERY)], result.retainedResourceBytes());
	backend->close();
	ASSERT_FALSE(domain.expired());
	EXPECT_EQ(result.getRows()[0].at("n").asNode().getProperties().at("payload"), PropertyValue(std::string(4000U, 'x')));
	// Cached plans now retain CACHE credit independently of the result.
    engine.resetEphemeralState();
	result = query::QueryResult{};
	EXPECT_TRUE(domain.expired());
}

TEST_F(RelocatableStorageBackendTest, NativeReadOnlyQueriesShareTheFileDomainWithoutWriting) {
	materializeNativeDatabase();
	auto writerOptions = backendOptions(); writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto writer = std::make_shared<RelocatableStorageBackend>(writerOptions); writer->open();
	(void)writer->createLogicalNodes({{{"Memory"}, {{"value", PropertyValue(7)}}}});
	auto reader = std::make_shared<RelocatableStorageBackend>(backendOptions()); reader->open();
	EXPECT_EQ(writer->queryResourceDomain(), reader->queryResourceDomain());
	const auto generation = reader->currentNativeMetadataGeneration(); reader->resetIoStats();
	query::QueryEngine engine(reader); auto result = engine.execute("MATCH (n:Memory) RETURN n.value AS value");
	ASSERT_EQ(result.rowCount(), 1U); EXPECT_GT(result.retainedResourceBytes(), 0U);
	EXPECT_EQ(reader->ioStats().data.write_bytes, 0U);
	EXPECT_EQ(reader->currentNativeMetadataGeneration(), generation);
	reader->close(); writer->close();
}

TEST_F(RelocatableStorageBackendTest, ReopenReusesTheDomainWhileAnOldResultRetainsItsCharge) {
	materializeNativeDatabase();
	auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
	query::QueryEngine engine(backend);
	auto first = engine.execute("RETURN 42 AS value");
	const auto bytes = first.retainedResourceBytes(); ASSERT_GT(bytes, 0U);
	auto domain = backend->queryResourceDomain(); backend->close(); backend->open();
	EXPECT_EQ(backend->queryResourceDomain(), domain);
	auto second = engine.execute("RETURN 43 AS value");
	EXPECT_EQ(domain->usage().classes[static_cast<size_t>(execution::detail::ResourceClass::RCK_QUERY)], bytes + second.retainedResourceBytes());
	first = query::QueryResult{};
	EXPECT_EQ(domain->usage().classes[static_cast<size_t>(execution::detail::ResourceClass::RCK_QUERY)], second.retainedResourceBytes());
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, HardLinkAliasesCannotCreateIndependentMemoryDomains) {
	materializeNativeDatabase();
	const auto alias = std::filesystem::path(data_path_.string() + ".memory-alias");
	struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{alias};
	std::error_code error; std::filesystem::create_hard_link(data_path_, alias, error);
	if (error) GTEST_SKIP() << "hard links unavailable: " << error.message();
	auto options = backendOptions(); RelocatableStorageBackend first(options);
	options.data_path = alias; RelocatableStorageBackend second(options);
	first.open(); second.open();
	EXPECT_EQ(first.queryResourceDomain(), second.queryResourceDomain());
	second.close(); first.close();
}

TEST_F(RelocatableStorageBackendTest, NativeEngineRejectsForeignQueryAccountsInsteadOfBypassingTheDomain) {
	using namespace execution::detail;
	materializeNativeDatabase();
	auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
	query::QueryEngine engine(backend);
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(10000U));
	auto foreign = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(10000U));
	query::QueryContext context; context.resource_account = std::make_shared<ResourceAccount>(foreign, ResourceClass::RCK_QUERY, 10000U);
	EXPECT_THROW((void)engine.execute("RETURN 42 AS value", context), std::invalid_argument);
	EXPECT_EQ(context.resource_account->used(), 0U);
	EXPECT_EQ(backend->queryResourceDomain()->usage().classes[static_cast<size_t>(execution::detail::ResourceClass::RCK_QUERY)], 0U);
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TransactionOverlayOutputUsesTheNativeDomainAndSurvivesRollback) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
	query::QueryEngine engine(backend, backend);
	auto transaction = backend->beginLogicalGraphTransaction(true);
	LogicalGraphTransactionOverlay overlay(*transaction);
	query::QueryContext context;
	auto result = engine.executePlanWithLogicalTransactionOverlay(engine.buildPlan("RETURN 42 AS value"), context, overlay, overlay);
	EXPECT_GT(result.retainedResourceBytes(), 0U);
	transaction->rollback(); backend->close();
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(42));
}

TEST_F(RelocatableStorageBackendTest, NativeCacheGrantIsChargedOnceAcrossReadOnlyAliases) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto first = std::make_shared<RelocatableStorageBackend>(backendOptions());
    auto second = std::make_shared<RelocatableStorageBackend>(backendOptions());
    first->open();
    auto domain = first->queryResourceDomain();
    const auto before = domain->usage();
    const size_t cacheClass = static_cast<size_t>(ResourceClass::RCK_CACHE);
    EXPECT_EQ(before.classes[cacheClass], 64U * 1024U * 1024U);
    second->open();
    EXPECT_EQ(second->queryResourceDomain(), domain);
    EXPECT_EQ(domain->usage().classes[cacheClass], before.classes[cacheClass]);
    first->close();
    EXPECT_EQ(domain->usage().classes[cacheClass], before.classes[cacheClass]);
    second->close();
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, RepeatedNativeReopenReturnsCacheAndOutputChargesToTheirOwners) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions());
    const auto process = ProcessResourceDomains::defaultOwner();
    const size_t initialUsage = process->usage().total;
    query::QueryResult retained;
    std::weak_ptr<DatabaseResourceBudget> weak;
    for (size_t iteration = 0U; iteration < 64U; ++iteration) {
        SCOPED_TRACE(iteration);
        backend->open();
        auto domain = backend->queryResourceDomain();
        if (iteration != 0U) EXPECT_EQ(domain, weak.lock());
        weak = domain;
        query::QueryEngine engine(backend);
        auto result = engine.execute("RETURN 42 AS value");
        retained = std::move(result);
        EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_QUERY)], retained.retainedResourceBytes());
        backend->close();
        EXPECT_GT(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 0U);
        engine.resetEphemeralState();
        EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 0U);
        EXPECT_EQ(process->usage().total, initialUsage + retained.retainedResourceBytes());
    }
    retained = query::QueryResult{};
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(process->usage().total, initialUsage);
}

TEST_F(RelocatableStorageBackendTest, CoordinatedSortJoinAndOptionalQuotaFailuresRefundBeforeRetry) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options);
    backend->open();
    for (char value : {'c', 'a', 'b'}) {
        static_cast<void>(backend->createLogicalNodes({{{"Batch"}, {{"payload", PropertyValue(std::string(1000U, value))}}}}));
    }
    query::QueryEngine engine(backend);
    const auto domain = backend->queryResourceDomain();
    const std::vector<std::pair<std::string, size_t>> cases{
        {"MATCH (n:Batch) WITH n ORDER BY n.payload RETURN n.payload AS payload", 3U},
        {"MATCH (n:Batch), (m:Batch) RETURN n.payload AS left_value, m.payload AS right_value", 9U},
        {"MATCH (n:Batch) OPTIONAL MATCH (m:Batch) RETURN n.payload AS left_value, m.payload AS right_value", 9U}};
    for (const auto &[cypher, rows] : cases) {
        SCOPED_TRACE(cypher);
        query::QueryContext context;
        auto limited = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 1024U);
        context.resource_account = limited;
        EXPECT_THROW(static_cast<void>(engine.execute(cypher, context)), ResourceAdmissionDenied);
        EXPECT_EQ(limited->used(), 0U);
        auto admitted = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 128U * 1024U);
        context.resource_account = admitted;
        {
            auto result = engine.execute(cypher, context);
            ASSERT_EQ(result.rowCount(), rows);
            EXPECT_GT(result.retainedResourceBytes(), 1000U);
            EXPECT_EQ(admitted->used(), result.retainedResourceBytes());
        }
        EXPECT_EQ(admitted->used(), 0U);
        EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_QUERY)], 0U);
    }
    engine.resetEphemeralState();
    backend->close();
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, CoordinatedPropertyPrefetchReleasesSelectedAndRejectedRowsBeforeClose) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options);
    backend->open();
    for (char value : {'c', 'a', 'b'}) {
        static_cast<void>(backend->createLogicalNodes({{{"Prefetch"}, {{"payload", PropertyValue(std::string(1000U, value))}}}}));
    }
    query::QueryEngine engine(backend);
    auto domain = backend->queryResourceDomain();
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 32U * 1024U);
    query::QueryContext context;
    context.resource_account = account;
    const std::string cypher = "MATCH (n:Prefetch) WHERE n.payload = $wanted RETURN n.payload AS payload";
    for (const bool matches : {true, false, true}) {
        SCOPED_TRACE(matches);
        context.parameters.insert_or_assign("wanted", PropertyValue(std::string(1000U, matches ? 'a' : 'z')));
        {
            auto result = engine.execute(cypher, context);
            ASSERT_EQ(result.rowCount(), matches ? 1U : 0U);
            if (matches) EXPECT_EQ(result.getRows()[0].at("payload").asPrimitive(), PropertyValue(std::string(1000U, 'a')));
            EXPECT_EQ(account->used(), result.retainedResourceBytes());
        }
        EXPECT_EQ(account->used(), 0U);
    }
    engine.resetEphemeralState();
    backend->close();
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, NativeReadFallbackInheritsTheSnapshotGrantWithoutAllocatingForExistingSessions) {
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
    auto snapshot = backend->openSnapshot();
    const auto &pin = snapshot.metadataSnapshot();
    auto memory = std::dynamic_pointer_cast<execution::detail::PrepaidMemoryResource>(pin.readContext()->memoryOwner());
    ASSERT_NE(memory, nullptr);
    NativeReadSession existing(pin);
    const size_t baseline = memory->used();
    {
        std::optional<NativeReadSession> fallback;
        for (size_t index = 0U; index < 100U; ++index)
            EXPECT_EQ(&selectNativeReadSession(pin, &existing, fallback), &existing);
        EXPECT_FALSE(fallback.has_value());
        EXPECT_EQ(memory->used(), baseline);
        auto &created = selectNativeReadSession(pin, nullptr, fallback);
        EXPECT_TRUE(fallback.has_value());
        EXPECT_GT(memory->used(), baseline);
        const size_t createdBytes = memory->used();
        EXPECT_EQ(&selectNativeReadSession(pin, nullptr, fallback), &created);
        EXPECT_EQ(memory->used(), createdBytes);
        created.putPropertyToken(std::string(4096U, 'p'), 1U);
        EXPECT_GT(memory->used(), createdBytes + 8192U);
    }
    EXPECT_EQ(memory->used(), baseline);
    snapshot.release();
    backend->close();
}

TEST_F(RelocatableStorageBackendTest, NativeTransactionWrappersRetainOnlyTheirLiveWriteClassObjectCharge) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    auto domain = backend->queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    for (bool readOnly : {false, true}) {
        auto transaction = backend->beginLogicalGraphTransaction(readOnly);
        const size_t objectBytes = domain->usage().classes[kind];
        EXPECT_GT(objectBytes, idleBytes); EXPECT_LT(objectBytes - idleBytes, 16U * 1024U);
        transaction->rollback();
        EXPECT_EQ(domain->usage().classes[kind], objectBytes);
        transaction.reset();
        EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    }
    auto committed = backend->beginLogicalGraphTransaction(true);
    const size_t objectBytes = domain->usage().classes[kind];
    (void)committed->commit();
    EXPECT_EQ(domain->usage().classes[kind], objectBytes);
    committed.reset();
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    backend->close();
}

TEST_F(RelocatableStorageBackendTest, DeniedTransactionObjectAdmissionLeaksNoSnapshotOrWriteCharge) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    auto domain = backend->queryResourceDomain();
    const size_t maximum = ProcessResourceDomains::databaseDefaults().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)];
    auto pressureAccount = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, maximum);
    ResourceLease pressure(pressureAccount); pressure.set(maximum - idleBytes);
    const auto generation = backend->currentNativeMetadataGeneration();
    EXPECT_THROW((void)backend->beginLogicalGraphTransaction(false), ResourceAdmissionDenied);
    EXPECT_THROW((void)backend->beginLogicalGraphTransaction(true), ResourceAdmissionDenied);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], maximum);
    EXPECT_EQ(backend->currentNativeMetadataGeneration(), generation);
    pressure.reset();
    auto retry = backend->beginLogicalGraphTransaction(false); retry->rollback(); retry.reset();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], idleBytes);
    EXPECT_NO_THROW(backend->close());
}

TEST_F(RelocatableStorageBackendTest, IssuedIdBuffersUseWriteQuotaAndDeniedStageKeepsPriorStatement) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    auto domain = backend->queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    auto transaction = backend->beginLogicalGraphTransaction(false);
    const size_t objectBytes = domain->usage().classes[kind];
    LogicalGraphMutationBatch first; first.node_creates.resize(1U);
    const auto firstResult = transaction->stageMutationBatch(std::move(first));
    ASSERT_EQ(firstResult.created_node_ids.size(), 1U);
    EXPECT_EQ(domain->usage().classes[kind], objectBytes + sizeof(uint64_t) + sizeof(NativeGraphLogicalIdLeaseRange) + sizeof(LogicalGraphNodeCreateRequest));
    const size_t retained = domain->usage().classes[kind];
    const size_t maximum = ProcessResourceDomains::databaseDefaults().classes[kind];
    auto pressureAccount = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, maximum);
    ResourceLease pressure(pressureAccount); pressure.set(maximum - retained);
    LogicalGraphMutationBatch denied; denied.node_creates.resize(1U);
    // Private input admission now precedes durable identity reservation.
    EXPECT_THROW((void)transaction->stageMutationBatch(std::move(denied)), execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
    EXPECT_EQ(domain->usage().classes[kind], maximum);
    pressure.reset();
    EXPECT_EQ(domain->usage().classes[kind], retained);
    LogicalGraphMutationBatch retry; retry.node_creates.resize(1U);
    const auto nextResult = transaction->stageMutationBatch(std::move(retry));
    ASSERT_EQ(nextResult.created_node_ids.size(), 1U);
    EXPECT_EQ(nextResult.created_node_ids.front(), firstResult.created_node_ids.front() + 1U);
    EXPECT_EQ(domain->usage().classes[kind], objectBytes + 2U * (sizeof(uint64_t) + sizeof(NativeGraphLogicalIdLeaseRange) + sizeof(LogicalGraphNodeCreateRequest)));
    const auto committed = transaction->commit();
    EXPECT_EQ(committed.created_node_ids, (std::vector<uint64_t>{firstResult.created_node_ids.front(), nextResult.created_node_ids.front()}));
    EXPECT_EQ(domain->usage().classes[kind], objectBytes);
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    backend->close();
}

TEST_F(RelocatableStorageBackendTest, ClosedTransactionObjectKeepsItsWriteDomainAfterBackendDestruction) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    std::weak_ptr<DatabaseResourceBudget> domain = backend->queryResourceDomain();
    auto transaction = backend->beginLogicalGraphTransaction(false);
    transaction->rollback();
    backend->close(); backend.reset();
    ASSERT_FALSE(domain.expired());
    EXPECT_GT(domain.lock()->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], 0U);
    EXPECT_EQ(domain.lock()->usage().classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 0U);
    transaction.reset();
    EXPECT_TRUE(domain.expired());
}

TEST_F(RelocatableStorageBackendTest, PreviewRangeQuotaFailureDoesNotPublishAnIncompletePreviewOrConsumeItsRetry) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    auto domain = backend->queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    auto transaction = backend->beginLogicalGraphTransaction(false);
    const size_t objectBytes = domain->usage().classes[kind];
    const size_t maximum = ProcessResourceDomains::databaseDefaults().classes[kind];
    auto pressureAccount = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, maximum);
    ResourceLease pressure(pressureAccount); pressure.set(maximum - objectBytes);
    LogicalGraphMutationBatch batch; batch.node_creates.resize(1U);
    EXPECT_THROW((void)transaction->previewMutationBatch(batch), ResourceAdmissionDenied);
    pressure.reset();
    const auto preview = transaction->previewMutationBatch(batch);
    ASSERT_EQ(preview.created_node_ids.size(), 1U);
    EXPECT_EQ(domain->usage().classes[kind], objectBytes + sizeof(NativeGraphLogicalIdLeaseRange));
    pressure.set(maximum - domain->usage().classes[kind]);
    EXPECT_THROW((void)transaction->stageMutationBatch(batch), execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(transaction->previewMutationBatch(batch), preview);
    pressure.reset();
    const auto staged = transaction->stageMutationBatch(batch);
    EXPECT_EQ(staged, preview);
    EXPECT_EQ(domain->usage().classes[kind], objectBytes + sizeof(uint64_t) + sizeof(NativeGraphLogicalIdLeaseRange) + sizeof(LogicalGraphNodeCreateRequest));
    transaction->rollback();
    EXPECT_EQ(domain->usage().classes[kind], objectBytes);
    transaction.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, TerminalRangeCleanupDoesNotCaptureAnUnrelatedAmbientAllocationOwner) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    auto transaction = backend->beginLogicalGraphTransaction(false);
    LogicalGraphMutationBatch batch; batch.node_creates.resize(1U);
    (void)transaction->stageMutationBatch(std::move(batch));
    auto ambient = std::make_shared<std::pmr::monotonic_buffer_resource>();
    std::weak_ptr<std::pmr::memory_resource> weak = ambient;
    {
        graph::utils::OwnedAllocationScope scope(ambient);
        transaction->rollback();
    }
    ambient.reset();
    EXPECT_TRUE(weak.expired());
    transaction.reset(); backend->close();
}

namespace {
class NativeReadDeniedAmbientResource final : public std::pmr::memory_resource {
    void *do_allocate(size_t, size_t) override { throw std::bad_alloc(); }
    void do_deallocate(void *, size_t, size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}
TEST_F(RelocatableStorageBackendTest, CachedTopologyReadDoesNotAllocateAnUnusedAmbientFallbackSession) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const auto ids = backend->createLogicalNodes({{{"Cached"}, {{"value", PropertyValue(int64_t{7})}}}});
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    // Topology-only hits need no Property materialization workspace.
    const EntityReadShape topology{};
    const auto original = snapshot->readNodeShaped(ids.front(), topology);
    std::optional<LogicalGraphNode> repeated;
    {
        NativeReadDeniedAmbientResource denied;
        struct RestoreDefault {
            std::pmr::memory_resource *previous;
            ~RestoreDefault() { std::pmr::set_default_resource(previous); }
        } restore{std::pmr::set_default_resource(&denied)};
        EXPECT_NO_THROW(repeated = snapshot->readNodeShaped(ids.front(), topology));
    }
    ASSERT_TRUE(repeated.has_value());
    EXPECT_EQ(repeated->node_id, original.node_id);
    EXPECT_EQ(repeated->labels, original.labels);
    EXPECT_EQ(repeated->properties, original.properties);
    snapshot.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, FullPropertyReadRestoresTheLazyDirectoryAllocationContext) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const auto ids = backend->createLogicalNodes({{{"Cached"}, {{"value", PropertyValue(std::string(8192U, 'n'))}}}, {{}, {}}});
    const auto edge = backend->createLogicalEdges({{ids[0], ids[1], "CachedType", {{"value", PropertyValue(std::string(8192U, 'e'))}}}}).front();
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    // Deferred directory initialization must keep its snapshot allocation domain.
    const EntityReadShape topology{.include_labels = true, .all_properties = true};
    const auto original = snapshot->readNodeShaped(ids.front(), topology);
    const auto originalEdge = snapshot->readEdgeShaped(edge, {.include_type = true, .all_properties = true});
    std::optional<LogicalGraphNode> repeated;
    std::optional<LogicalGraphEdge> repeatedEdge;
    {
        NativeReadDeniedAmbientResource denied;
        struct RestoreDefault {
            std::pmr::memory_resource *previous;
            ~RestoreDefault() { std::pmr::set_default_resource(previous); }
        } restore{std::pmr::set_default_resource(&denied)};
        EXPECT_NO_THROW(repeated = snapshot->readNodeShaped(ids.front(), topology));
        EXPECT_NO_THROW(repeatedEdge = snapshot->readEdgeShaped(edge, {.include_type = true, .all_properties = true}));
    }
    ASSERT_TRUE(repeated.has_value()); ASSERT_TRUE(repeatedEdge.has_value());
    EXPECT_EQ(*repeatedEdge, originalEdge);
    EXPECT_EQ(repeated->node_id, original.node_id);
    EXPECT_EQ(repeated->labels, original.labels);
    EXPECT_EQ(repeated->properties, original.properties);
    snapshot.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, RetainedPropertyPatchSlotsUseWriteBudgetAndReleaseAtRollback) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphNodeCreateRequest> nodes(32U);
    const auto ids = backend.createLogicalNodes(nodes);
    auto domain = backend.queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const auto wrapper = domain->usage().classes[kind];
    std::vector<LogicalGraphPropertyPatchRequest> patches;
    for (const auto id : ids)
        patches.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id}, {{"rank", PropertyValue(1)}}, {}});
    (void)transaction->stageMutationBatchWithPropertyPatches({}, std::move(patches), ids.size());
    EXPECT_GE(domain->usage().classes[kind] - wrapper, ids.size() * sizeof(LogicalGraphPropertyPatchRequest));
    transaction->rollback();
    EXPECT_EQ(domain->usage().classes[kind], wrapper);
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, DeniedPropertyPatchSlotCopyPreservesPriorStatementAndCanCommit) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphNodeCreateRequest> nodes(32U);
    for (auto &node : nodes) node.properties.push_back({"rank", PropertyValue(0)});
    const auto ids = backend.createLogicalNodes(nodes);
    const auto id = ids.front();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const LogicalGraphMutationEntityReference entity{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
    std::vector<LogicalGraphPropertyPatchRequest> patches;
    for (const auto target : ids)
        patches.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
            LogicalGraphMutationReferenceKind::LGRK_PERSISTED, target}, {{"rank", PropertyValue(1)}}, {}});
    // Entry bound includes each persisted source field and each patch field.
    (void)transaction->stageMutationBatchWithPropertyPatches({}, std::move(patches), 2U * ids.size());
    auto domain = backend.queryResourceDomain();
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, 128U * 1024U * 1024U);
    ResourceLease pressure(account);
    // Leave room for validation, but not for admitting the retained nested
    // payload and slot copy. The private-copy preflight rejects before cloning.
    constexpr size_t validationHeadroom = 512U;
    ASSERT_GT(account->available(), validationHeadroom);
    pressure.set(account->available() - validationHeadroom);
    EXPECT_THROW((void)transaction->stageMutationBatchWithPropertyPatches(
        {}, {{entity, {{"rank", PropertyValue(2)}}, {}}}, 2U), execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
    pressure.reset();
    EXPECT_NO_THROW((void)transaction->commit());
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], idleBytes);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readNodeProperty(id, "rank"), PropertyValue(1));
    snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, DeniedCanonicalCreationWorkspaceRollsBackWithoutPublishingNodes) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    LogicalGraphMutationBatch batch; batch.node_creates.resize(32U);
    const auto staged = transaction->stageMutationBatch(std::move(batch));
    ASSERT_EQ(staged.created_node_ids.size(), 32U);
    auto domain = backend.queryResourceDomain();
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, 128U * 1024U * 1024U);
    ResourceLease pressure(account); pressure.set(account->available());
    EXPECT_THROW((void)transaction->commit(), ResourceAdmissionDenied);
    EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
    pressure.reset(); transaction.reset();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], idleBytes);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    for (const auto id : staged.created_node_ids) EXPECT_FALSE(snapshot->containsNode(id));
    snapshot.reset();
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    auto retry = backend.beginLogicalGraphTransaction(false);
    LogicalGraphMutationBatch next; next.node_creates.resize(1U);
    const auto ids = retry->stageMutationBatch(std::move(next)).created_node_ids;
    EXPECT_GT(ids.front(), staged.created_node_ids.back());
    EXPECT_NO_THROW((void)retry->commit());
    retry.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, RetainedMutationSlotsAreChargedToWriteBeforeStagingAndFreedAtRollback) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto domain = backend.queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const auto wrapper = domain->usage().classes[kind];
    LogicalGraphMutationBatch batch; batch.node_creates.resize(64U);
    const auto ids = transaction->stageMutationBatch(std::move(batch)).created_node_ids;
    EXPECT_GE(domain->usage().classes[kind] - wrapper, 64U * sizeof(LogicalGraphNodeCreateRequest));
    transaction->rollback();
    EXPECT_EQ(domain->usage().classes[kind], wrapper);
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    for (const auto id : ids) EXPECT_FALSE(snapshot->containsNode(id));
    snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayNodesKeepWriteOwnershipAfterRollbackUntilOverlayDestruction) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto id = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
    auto domain = backend.queryResourceDomain();
    const size_t kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    auto transaction = backend.beginLogicalGraphTransaction(false);
    const auto wrapper = domain->usage().classes[kind];
    {
        LogicalGraphTransactionOverlay overlay(*transaction);
        overlay.replaceLogicalNodeProperties({{id, {{"rank", PropertyValue(1)}}}});
        EXPECT_GT(domain->usage().classes[kind], wrapper);
        transaction->rollback();
        // The overlay still owns its retained node even though native staging
        // has released its request slots and base snapshot.
        EXPECT_GT(domain->usage().classes[kind], wrapper);
    }
    EXPECT_EQ(domain->usage().classes[kind], wrapper);
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[kind], idleBytes);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, DeniedOverlayUndoMetadataPreservesThePriorVisibleStatement) {
    using namespace execution::detail;
    constexpr size_t idleBytes = ProcessResourceDomains::LOGICAL_ID_CAPABILITY_GRANT_BYTES;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto id = backend.createLogicalNodes({{{}, {{"rank", PropertyValue(0)}}}}).front();
    auto domain = backend.queryResourceDomain();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    {
        LogicalGraphTransactionOverlay overlay(*transaction);
        overlay.replaceLogicalNodeProperties({{id, {{"rank", PropertyValue(1)}}}});
        auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, 128U * 1024U * 1024U);
        ResourceLease pressure(account); pressure.set(account->available());
        EXPECT_THROW(overlay.replaceLogicalNodeProperties({{id, {{"rank", PropertyValue(2)}}}}), ResourceAdmissionDenied);
        EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
        pressure.reset();
        auto visible = overlay.openLogicalGraphReadSnapshot();
        EXPECT_EQ(visible->readNodeProperty(id, "rank"), PropertyValue(1));
        visible.reset();
        EXPECT_NO_THROW((void)transaction->commit());
    }
    transaction.reset();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], idleBytes);
    auto durable = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(durable->readNodeProperty(id, "rank"), PropertyValue(1));
    durable.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, DeniedOverlayCreationUndoPreservesItsPreviewIdentityForRetry) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    uint64_t created = 0U;
    {
        LogicalGraphTransactionOverlay overlay(*transaction);
        LogicalGraphMutationBatch batch; batch.node_creates.resize(1U);
        const auto preview = transaction->previewMutationBatch(batch);
        auto domain = backend.queryResourceDomain();
        auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, 128U * 1024U * 1024U);
        ResourceLease pressure(account); pressure.set(account->available());
        EXPECT_THROW((void)overlay.executeLogicalGraphMutationBatch(batch), ResourceAdmissionDenied);
        EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
        pressure.reset();
        const auto retried = overlay.executeLogicalGraphMutationBatch(batch);
        EXPECT_EQ(retried, preview);
        ASSERT_EQ(retried.created_node_ids.size(), 1U);
        created = retried.created_node_ids.front();
        EXPECT_NO_THROW((void)transaction->commit());
    }
    transaction.reset();
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_TRUE(snapshot->containsNode(created));
    snapshot.reset();
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalIdCapabilityGrantIsSharedAcrossWritableFileAliases) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend first(options), second(options); first.open(); second.open();
    auto domain = first.queryResourceDomain();
    auto memory = ProcessResourceDomains::defaultOwner()->logicalIdGrant(domain);
    EXPECT_EQ(domain, second.queryResourceDomain());
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], memory->capacity());
    const auto baseline = memory->used();
    auto one = first.reserveLogicalGraphCreationIds(1U, 1U);
    auto two = second.reserveLogicalGraphCreationIds(1U, 1U);
    EXPECT_GT(memory->used(), baseline);
    first.abandonLogicalGraphCreationIds(one); second.abandonLogicalGraphCreationIds(two);
    EXPECT_EQ(memory->used(), baseline);
    first.close(); second.close();
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], memory->capacity());
    memory.reset();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], 0U);
}

TEST_F(RelocatableStorageBackendTest, LogicalIdCapabilityQuotaRejectsBeforePublicationAndCanRetry) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto domain = backend.queryResourceDomain();
    auto memory = ProcessResourceDomains::defaultOwner()->logicalIdGrant(domain);
    auto warm = backend.reserveLogicalGraphCreationIds(1U, 1U);
    backend.abandonLogicalGraphCreationIds(warm);
    const auto baseline = memory->used();
    const auto bytes = memory->capacity() - baseline;
    auto release = [memory, bytes](void *pointer) { memory->deallocate(pointer, bytes); };
    std::unique_ptr<void, decltype(release)> pressure(memory->allocate(bytes), release);
    const auto generation = backend.currentNativeMetadataGeneration();
    backend.resetIoStats();
    EXPECT_THROW((void)backend.reserveLogicalGraphCreationIds(1U, 1U), ResourceAdmissionDenied);
    EXPECT_EQ(backend.ioStats().data.write_bytes, 0U);
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
    pressure.reset();
    EXPECT_EQ(memory->used(), baseline);
    auto retry = backend.reserveLogicalGraphCreationIds(1U, 1U);
    backend.abandonLogicalGraphCreationIds(retry);
    EXPECT_EQ(memory->used(), baseline);
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, PhysicalReadonlyOpenDoesNotPrepayAWriteCapabilityGrant) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto domain = backend.queryResourceDomain();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], 0U);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeIntegrityWorkspaceAdmissionUsesMaintenanceAndRefundsAfterScan) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    (void)backend->createLogicalNodes({{{"Scan"}, {{"rank", PropertyValue(7)}}}});
    auto domain = backend->queryResourceDomain();
    constexpr auto maintenance = static_cast<size_t>(ResourceClass::RCK_MAINTENANCE);
    const auto baseline = domain->usage().classes[maintenance];
    const auto before = backend->verifyIntegrity();
    EXPECT_EQ(domain->usage().classes[maintenance], baseline);
    constexpr size_t ceiling = 32U * 1024U * 1024U;
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_MAINTENANCE, ceiling);
    ResourceLease pressure(account); pressure.set(ceiling - baseline);
    const auto generation = backend->currentNativeMetadataGeneration();
    backend->resetIoStats();
    EXPECT_THROW(static_cast<void>(backend->verifyIntegrity()), ResourceAdmissionDenied);
    EXPECT_EQ(domain->usage().classes[maintenance], ceiling);
    EXPECT_EQ(backend->ioStats().data.write_calls, 0U);
    EXPECT_EQ(backend->currentNativeMetadataGeneration(), generation);
    pressure.reset();
    EXPECT_EQ(backend->verifyIntegrity(), before);
    EXPECT_EQ(domain->usage().classes[maintenance], baseline);
    backend->close();
}

TEST_F(RelocatableStorageBackendTest, NativeIntegrityWorkspaceExhaustionPreservesReadOnlySnapshot) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto writerOptions = backendOptions(); writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend writer(writerOptions); writer.open();
    (void)writer.createLogicalNodes({{{"Scan"}, {{"rank", PropertyValue(7)}}}});
    const auto expected = writer.verifyIntegrity(); writer.close();
    auto options = backendOptions(); options.maximum_integrity_workspace_bytes = 256U;
    RelocatableStorageBackend reader(options); reader.open();
    const auto generation = reader.currentNativeMetadataGeneration();
    const auto baseline = reader.queryResourceDomain()->usage();
    reader.resetIoStats();
    EXPECT_THROW(static_cast<void>(reader.verifyIntegrity()), ResourceAdmissionDenied);
    EXPECT_EQ(reader.queryResourceDomain()->usage().total, baseline.total);
    EXPECT_EQ(reader.ioStats().data.write_calls, 0U);
    EXPECT_EQ(reader.currentNativeMetadataGeneration(), generation);
    reader.close();
    RelocatableStorageBackend retry(backendOptions()); retry.open();
    EXPECT_EQ(retry.verifyIntegrity(), expected); retry.close();
}

TEST_F(RelocatableStorageBackendTest, NativeIntegrityRetainsMaintenanceHeadroomUnderSaturatedForegroundAdmission) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"Scan"}, {{"rank", PropertyValue(7)}}}});
    const auto expected = backend.verifyIntegrity();
    auto domain = backend.queryResourceDomain();
    ResourceAccount query(domain, ResourceClass::RCK_QUERY, 192U * 1024U * 1024U);
    ResourceLease foreground(query); foreground.set(query.available());
    const auto saturated = domain->usage().total;
    EXPECT_EQ(query.available(), 0U);
    EXPECT_EQ(backend.verifyIntegrity(), expected);
    EXPECT_EQ(domain->usage().total, saturated);
    EXPECT_EQ(query.available(), 0U);
    foreground.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeIntegrityScanDoesNotSpendOrEvictExhaustedCacheCredit) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"Scan"}, {{"rank", PropertyValue(7)},
        {"payload", PropertyValue(std::string(12000U, 'm'))},
        {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)})}}}});
    (void)backend.createLogicalConstraint({"scan_rank_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, "Scan", {"rank"}, LogicalGraphConstraintPropertyType::LGCPT_NONE});
    (void)backend.createLogicalNodeVectorIndex({"scan_embedding", "Scan", "embedding", 2U, "l2"});
    const auto expected = backend.verifyIntegrity();
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(backend.queryResourceDomain());
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        EXPECT_EQ(cache->used(), cache->capacity());
        EXPECT_EQ(backend.verifyIntegrity(), expected);
        EXPECT_EQ(cache->used(), cache->capacity());
    }
    EXPECT_EQ(backend.verifyIntegrity(), expected);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeFileLimitRejectsWritableOversizeButAllowsPhysicalReadonlyInspection) {
    materializeNativeDatabase();
    const auto fileBytes = std::filesystem::file_size(data_path_);
    ASSERT_GT(fileBytes, 1U);
    auto options = backendOptions(); options.maximum_database_file_bytes = fileBytes - 1U;
    RelocatableStorageBackend reader(options); reader.open();
    reader.resetIoStats();
    const auto report = reader.verifyIntegrity();
    EXPECT_EQ(reader.ioStats().data.write_calls, 0U);
    reader.close();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend writer(options);
    try { writer.open(); FAIL() << "oversized writable database was admitted"; }
    catch (const std::system_error &error) { EXPECT_EQ(error.code(), std::make_error_code(std::errc::no_space_on_device)); }
    EXPECT_EQ(std::filesystem::file_size(data_path_), fileBytes);
    RelocatableStorageBackend retry(backendOptions()); retry.open();
    EXPECT_EQ(retry.verifyIntegrity(), report); retry.close();
}

TEST_F(RelocatableStorageBackendTest, NativePinnedSnapshotFilePressureFailsWithoutPublishingAndReusesSpaceAfterRelease) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    uint64_t nodeId = 0U;
    {
        RelocatableStorageBackend seed(options); seed.open();
        nodeId = seed.createLogicalNodes({{{"Pinned"}, {{"rank", PropertyValue(0)},
            {"payload", PropertyValue(std::string(6000U, 'p'))}}}}).front();
        seed.close();
    }
    const auto ceiling = std::filesystem::file_size(data_path_) + 2U * RSS_SEGMENT_SIZE;
    options.maximum_database_file_bytes = ceiling;
    RelocatableStorageBackend backend(options); backend.open();
    auto old = backend.openLogicalGraphReadSnapshot();
    size_t completed = 0U; bool denied = false;
    for (size_t iteration = 1U; iteration <= 128U; ++iteration) {
        const auto generation = backend.currentNativeMetadataGeneration();
        try {
            backend.replaceLogicalNodeProperties({{nodeId, {{"rank", PropertyValue(static_cast<int64_t>(iteration))},
                {"payload", PropertyValue(std::string(6000U, 'p'))}}}});
            completed = iteration;
        } catch (const std::system_error &error) {
            ASSERT_EQ(error.code(), std::make_error_code(std::errc::no_space_on_device));
            EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
            denied = true; break;
        }
        ASSERT_LE(std::filesystem::file_size(data_path_), ceiling);
    }
    ASSERT_TRUE(denied) << "pinned write workload did not reach the configured file ceiling";
    ASSERT_GT(completed, 0U);
    EXPECT_LE(std::filesystem::file_size(data_path_), ceiling);
    EXPECT_EQ(old->readNodeProperty(nodeId, "rank"), PropertyValue(0));
    EXPECT_EQ(old->readNodeProperty(nodeId, "payload"), PropertyValue(std::string(6000U, 'p')));
    {
        auto latest = backend.openLogicalGraphReadSnapshot();
        EXPECT_EQ(latest->readNodeProperty(nodeId, "rank"),
            PropertyValue(static_cast<int64_t>(completed)));
    }
    old.reset(); backend.close();
    RelocatableStorageBackend retry(options); retry.open();
    LargeValueReachabilityReclaimOptions reclaim;
    reclaim.maximum_manifest_locations = 256U; reclaim.maximum_manifest_metadata_pages = 256U;
    reclaim.maximum_manifest_nodes = 256U; reclaim.maximum_owner_locations = 256U;
    reclaim.maximum_owner_metadata_pages = 256U; reclaim.maximum_data_pages = 1024U;
    reclaim.maximum_retirements = 1024U; reclaim.maximum_page_locations = 4096U;
    reclaim.maximum_pools = 64U; reclaim.first_index_slot = PhysicalSlotId(1U);
    reclaim.last_index_slot = PhysicalSlotId(999U);
    uint64_t reclaimed = 0U;
    for (size_t pass = 0U; pass < 8U; ++pass) {
        reclaimed += retry.reclaimUnreachableLargeValues(reclaim).retired_values;
        ASSERT_LE(std::filesystem::file_size(data_path_), ceiling);
    }
    EXPECT_GT(reclaimed, 0U);
    retry.replaceLogicalNodeProperties({{nodeId, {{"rank", PropertyValue(1000)},
        {"payload", PropertyValue(std::string(6000U, 'p'))}}}});
    EXPECT_LE(std::filesystem::file_size(data_path_), ceiling);
    EXPECT_NO_THROW(static_cast<void>(retry.verifyIntegrity()));
    retry.close();
    RelocatableStorageBackend reader(backendOptions()); reader.open();
    auto final = reader.openLogicalGraphReadSnapshot();
    EXPECT_EQ(final->readNodeProperty(nodeId, "rank"), PropertyValue(1000));
    final.reset(); reader.close();
}

TEST_F(RelocatableStorageBackendTest, NativeFileLimitValidatesBeforeCreationAndCleansUpDeniedBootstrap) {
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    options.data_path = data_path_.string() + ".quota-new";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{options.data_path};
    ASSERT_FALSE(std::filesystem::exists(options.data_path));
    const auto owner = execution::detail::ProcessResourceDomains::defaultOwner();
    const auto before = owner->usage().total;
    options.maximum_database_file_bytes = 0U;
    EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(options.data_path));
    options.maximum_database_file_bytes = (std::numeric_limits<uint64_t>::max)();
    EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(options.data_path));
    options.maximum_database_file_bytes = 1U;
    EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), std::system_error);
    EXPECT_FALSE(std::filesystem::exists(options.data_path));
    EXPECT_EQ(owner->usage().total, before);
}

TEST_F(RelocatableStorageBackendTest, NativeWritableAliasesRejectConflictingFileLimitsBeforeMetadataWrites) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    options.maximum_database_file_bytes = std::filesystem::file_size(data_path_) + 8U * RSS_SEGMENT_SIZE;
    RelocatableStorageBackend first(options); first.open();
    const auto generation = first.currentNativeMetadataGeneration();
    const auto fileBytes = std::filesystem::file_size(data_path_);
    const auto alias = std::filesystem::path(data_path_.string() + ".file-limit-alias");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{alias};
    std::error_code error; std::filesystem::create_hard_link(data_path_, alias, error);
    if (error) GTEST_SKIP() << "hard links unavailable: " << error.message();
    auto otherOptions = options; otherOptions.data_path = alias;
    otherOptions.maximum_database_file_bytes += RSS_SEGMENT_SIZE;
    RelocatableStorageBackend conflicting(otherOptions);
    EXPECT_THROW(conflicting.open(), std::invalid_argument);
    EXPECT_EQ(first.currentNativeMetadataGeneration(), generation);
    EXPECT_EQ(std::filesystem::file_size(data_path_), fileBytes);
    RelocatableStorageBackend matching(options); EXPECT_NO_THROW(matching.open()); matching.close();
    first.close();
    EXPECT_NO_THROW(conflicting.open()); conflicting.close();
}

TEST_F(RelocatableStorageBackendTest, NativeLargeValueReclaimAdmissionUsesMaintenanceBeforePublication) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    auto domain = backend.queryResourceDomain();
    constexpr auto maintenance = static_cast<size_t>(ResourceClass::RCK_MAINTENANCE);
    const auto baseline = domain->usage().classes[maintenance];
    constexpr size_t ceiling = 32U * 1024U * 1024U;
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_MAINTENANCE, ceiling);
    ResourceLease pressure(account); pressure.set(ceiling - baseline);
    LargeValueReachabilityReclaimOptions reclaim;
    reclaim.maximum_manifest_locations = 256U; reclaim.maximum_manifest_metadata_pages = 256U;
    reclaim.maximum_manifest_nodes = 256U; reclaim.maximum_owner_locations = 256U;
    reclaim.maximum_owner_metadata_pages = 256U; reclaim.maximum_data_pages = 1024U;
    reclaim.maximum_retirements = 1024U; reclaim.maximum_page_locations = 4096U;
    reclaim.first_index_slot = PhysicalSlotId(1U); reclaim.last_index_slot = PhysicalSlotId(999U);
    const auto generation = backend.currentNativeMetadataGeneration();
    backend.resetIoStats();
    EXPECT_THROW(static_cast<void>(backend.reclaimUnreachableLargeValues(reclaim)), ResourceAdmissionDenied);
    EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
    pressure.reset();
    EXPECT_NO_THROW(static_cast<void>(backend.reclaimUnreachableLargeValues(reclaim)));
    EXPECT_EQ(domain->usage().classes[maintenance], baseline);
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeLargeValueReclaimUsesProtectedWorkspaceAndRejectsExhaustionWithoutPublication) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"Retain"}, {{"payload", PropertyValue(std::string(12000U, 'r'))}}}});
    LargeValueReachabilityReclaimOptions reclaim;
    reclaim.maximum_manifest_locations = 256U; reclaim.maximum_manifest_metadata_pages = 256U;
    reclaim.maximum_manifest_nodes = 256U; reclaim.maximum_owner_locations = 256U;
    reclaim.maximum_owner_metadata_pages = 256U; reclaim.maximum_data_pages = 1024U;
    reclaim.maximum_retirements = 1024U; reclaim.maximum_page_locations = 4096U;
    reclaim.first_index_slot = PhysicalSlotId(1U); reclaim.last_index_slot = PhysicalSlotId(999U);
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(backend.queryResourceDomain());
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        const auto generation = backend.currentNativeMetadataGeneration();
        backend.resetIoStats();
        for (size_t pass = 0U; pass < 4U; ++pass) {
            const auto report = backend.reclaimUnreachableLargeValues(reclaim);
            EXPECT_GT(report.reachable_values, 0U);
            EXPECT_EQ(report.retired_values, 0U);
            EXPECT_EQ(cache->used(), cache->capacity());
        }
        EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
        EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    }
    backend.close();
    cache.reset();
    auto small = options; small.maximum_reclaim_workspace_bytes = 256U;
    RelocatableStorageBackend constrained(small); constrained.open();
    const auto generation = constrained.currentNativeMetadataGeneration();
    constrained.resetIoStats();
    EXPECT_THROW(static_cast<void>(constrained.reclaimUnreachableLargeValues(reclaim)), ResourceAdmissionDenied);
    EXPECT_EQ(constrained.currentNativeMetadataGeneration(), generation);
    EXPECT_EQ(constrained.ioStats().data.write_calls, 0U);
    EXPECT_FALSE(constrained.hasIndeterminateWriteOutcome());
    constrained.close();
    RelocatableStorageBackend retry(options); retry.open();
    EXPECT_GT(retry.reclaimUnreachableLargeValues(reclaim).reachable_values, 0U);
    EXPECT_NO_THROW(static_cast<void>(retry.verifyIntegrity()));
    retry.close();
}

TEST_F(RelocatableStorageBackendTest, NativeMaintenanceWorkspacePressureDefersWithoutFencingAndResumesAfterRefund) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    options.maintenance = RelocatableMaintenanceOptions{};
    options.maintenance->loop.minimum_cycle_interval = std::chrono::milliseconds(1);
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"Point"}, {{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)})}}}});
    (void)backend.createLogicalNodeVectorIndex({"point_embedding", "Point", "embedding", 2U, "l2"});
    backend.pauseMaintenance();
    auto domain = backend.queryResourceDomain();
    constexpr auto maintenance = static_cast<size_t>(ResourceClass::RCK_MAINTENANCE);
    const auto baseline = domain->usage().classes[maintenance];
    constexpr size_t ceiling = 32U * 1024U * 1024U;
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_MAINTENANCE, ceiling);
    ResourceLease pressure(account); pressure.set(ceiling - baseline);
    const auto attempts = backend.maintenanceMetrics().loop.deferred_attempts;
    const auto scanned = backend.maintenanceMetrics().vector_rebuild_recovery_scanned_metadata_pages;
    const auto generation = backend.currentNativeMetadataGeneration();
    backend.resetIoStats(); backend.resumeMaintenance();
    const auto wait = [](const auto &predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return predicate();
    };
    EXPECT_TRUE(wait([&] { return backend.maintenanceMetrics().loop.deferred_attempts >= attempts + 2U; }));
    backend.pauseMaintenance();
    EXPECT_EQ(backend.maintenanceMetrics().loop.failed_tasks, 0U);
    EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    EXPECT_EQ(domain->usage().classes[maintenance], ceiling);
    pressure.reset(); backend.resumeMaintenance();
    EXPECT_TRUE(wait([&] { return backend.maintenanceMetrics().vector_rebuild_recovery_scanned_metadata_pages > scanned; }));
    backend.pauseMaintenance();
    EXPECT_EQ(backend.maintenanceMetrics().loop.failed_tasks, 0U);
    EXPECT_EQ(domain->usage().classes[maintenance], baseline);
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeEngineRejectsOtherResourceClassesWithinItsOwnDomain) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
    query::QueryEngine engine(backend);
    const auto generation = backend->currentNativeMetadataGeneration();
    for (const auto kind : {ResourceClass::RCK_CACHE, ResourceClass::RCK_WRITE, ResourceClass::RCK_MAINTENANCE, ResourceClass::RCK_CODE}) {
        query::QueryContext context;
        context.resource_account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(), kind, 65536U);
        backend->resetIoStats();
        EXPECT_THROW(static_cast<void>(engine.execute("RETURN 42 AS value", context)), std::invalid_argument);
        EXPECT_EQ(context.resource_account->used(), 0U); EXPECT_EQ(backend->ioStats().data.write_calls, 0U);
        EXPECT_EQ(backend->currentNativeMetadataGeneration(), generation);
    }
    backend->close();
}

TEST_F(RelocatableStorageBackendTest, NativeWorkspaceLimitsRejectZeroBeforeCreatingAFile) {
    auto base = backendOptions(); base.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    base.data_path = data_path_.string() + ".workspace-zero";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{base.data_path};
    for (const bool integrity : {false, true}) {
        auto options = base;
        if (integrity) options.maximum_integrity_workspace_bytes = 0U;
        else options.maximum_reclaim_workspace_bytes = 0U;
        EXPECT_THROW(static_cast<void>(RelocatableStorageBackend::initializeNew(options)), std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(options.data_path));
    }
}

TEST_F(RelocatableStorageBackendTest, NestedOverlayPayloadsKeepWriteCreditAfterNativeRollbackUntilOverlayDestruction) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto ids = backend.createLogicalNodes({{{}, {}}, {{}, {}}});
    auto transaction = backend.beginLogicalGraphTransaction(false);
    auto budget = std::dynamic_pointer_cast<execution::MaterializedByteBudget>(
        dynamic_cast<LogicalGraphTransactionMemoryProvider &>(*transaction).retainedWriteMemory());
    ASSERT_NE(budget, nullptr);
    const auto wrapper = budget->used();
    {
        LogicalGraphTransactionOverlay overlay(*transaction);
        const PropertyValue expected(std::string(65536U, 'p'));
        overlay.replaceLogicalNodeProperties({{ids[0], {{"payload", expected}}}});
        const auto edges = overlay.createLogicalEdges({{ids[0], ids[1], std::string(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES, 't'), {{"payload", expected}}}});
        ASSERT_EQ(edges.size(), 1U);
        EXPECT_GE(budget->used(), 2U * 65536U + RSS_SYSTEM_CATALOG_NAME_MAX_BYTES);
        transaction->rollback();
        // Native drafts are gone; the separate read-your-writes owners remain.
        EXPECT_GE(budget->used(), 2U * 65536U + RSS_SYSTEM_CATALOG_NAME_MAX_BYTES);
    }
    EXPECT_LE(budget->used(), wrapper);
    EXPECT_LT(budget->used(), 16384U);
    transaction.reset(); EXPECT_EQ(budget->used(), 0U);
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close();
}

TEST_F(RelocatableStorageBackendTest, EscapedCompiledAndCacheHitPlansRetainQueryCreditUntilTheirNodesAreDestroyed) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
    auto domain = backend->queryResourceDomain();
    const auto queryClass = static_cast<size_t>(ResourceClass::RCK_QUERY);
    const auto cacheClass = static_cast<size_t>(ResourceClass::RCK_CACHE);
    query::QueryEngine engine(backend);
    std::optional<query::QueryPlan> first(engine.buildPlan("RETURN 42 AS value"));
    const size_t firstBytes = domain->usage().classes[queryClass];
    ASSERT_GT(firstBytes, 0U);
    std::optional<query::QueryPlan> second(engine.buildPlan("RETURN 42 AS value"));
    ASSERT_GT(engine.getPlanCache().hits(), 0U);
    EXPECT_GT(domain->usage().classes[queryClass], firstBytes);
    backend->close();
    engine.resetEphemeralState();
    EXPECT_EQ(domain->usage().classes[cacheClass], 0U);
    EXPECT_GT(domain->usage().classes[queryClass], firstBytes);
    EXPECT_FALSE(first->root->toString().empty());
    first.reset();
    EXPECT_GT(domain->usage().classes[queryClass], 0U);
    EXPECT_FALSE(second->root->toString().empty());
    second.reset();
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, CompilationSharesTheCallersQueryAccountAndRefundsBeforeReturningItsResult) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
    auto domain = backend->queryResourceDomain();
    query::QueryEngine engine(backend);
    query::QueryContext context;
    context.resource_account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 1U);
    EXPECT_THROW(static_cast<void>(engine.execute("RETURN 42 AS value", context)), ResourceAdmissionDenied);
    EXPECT_EQ(context.resource_account->used(), 0U);
    EXPECT_EQ(engine.getPlanCache().size(), 0U);
    context.resource_account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 16384U);
    {
        const auto result = engine.execute("RETURN 42 AS value", context);
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(42));
        EXPECT_EQ(context.resource_account->used(), result.retainedResourceBytes());
    }
    EXPECT_EQ(context.resource_account->used(), 0U);
    engine.resetEphemeralState(); backend->close();
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, SchemaResultsUseSnapshotCacheAuthorityWithoutAnExplicitCallerScope) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    static_cast<void>(backend.createLogicalNodeIndex({std::string(1000U, 'i'), "Memory", {"value"}}));
    auto cache = std::dynamic_pointer_cast<execution::detail::PrepaidMemoryResource>(backend.logicalPlanCacheMemoryOwner());
    ASSERT_NE(cache, nullptr);
    {
        auto snapshot = backend.openSnapshot();
        std::optional<graph::utils::OwnedStdVector<IndexDefinition>> definitions;
        definitions.emplace(backend.readIndexDefinitions(snapshot, 16U));
        ASSERT_FALSE(definitions->empty());
        EXPECT_TRUE(std::ranges::any_of(*definitions, [](const auto &definition) { return definition.name == std::string(1000U, 'i'); }));
        using namespace graph::utils::detail;
        size_t retained = standardVectorHeapBufferBytes(*definitions);
        for (const auto &definition : *definitions) {
            retained += dynamicStringBufferBytes(definition.name) + standardVectorHeapBufferBytes(definition.property_names) +
                dynamicStringBufferBytes(definition.vector_metric);
            for (const auto &property : definition.property_names) retained += dynamicStringBufferBytes(property);
        }
        const auto before = cache->used();
        definitions.reset();
        EXPECT_EQ(cache->used() + retained, before);
    }
    backend.close(); EXPECT_EQ(cache->used(), 0U);
}

TEST_F(RelocatableStorageBackendTest, SchemaQueryOutputKeepsOnlyItsQueryStorageAfterBackendClose) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    const std::string name(1000U, 's');
    static_cast<void>(backend->createLogicalNodeIndex({name, "Memory", {"value"}}));
    const auto domain = backend->queryResourceDomain();
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 65536U);
    query::QueryEngine engine(backend);
    query::QueryContext context; context.resource_account = account;
    {
        const auto result = engine.execute("SHOW INDEXES", context);
        ASSERT_FALSE(result.isEmpty());
        EXPECT_GT(result.retainedResourceBytes(), name.size());
        EXPECT_EQ(account->used(), result.retainedResourceBytes());
        backend->close(); engine.resetEphemeralState();
        EXPECT_TRUE(std::ranges::any_of(result.getRows(), [&](const auto &row) {
            return row.at("name").asPrimitive() == PropertyValue(name);
        }));
        EXPECT_EQ(account->used(), result.retainedResourceBytes());
    }
    EXPECT_EQ(account->used(), 0U);
}

TEST_F(RelocatableStorageBackendTest, SchemaWritePressureRejectsBeforePublicationAndReleasesItsWorkspace) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto domain = backend.queryResourceDomain();
    const size_t writeKind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    const size_t baseline = domain->usage().classes[writeKind];
    auto pressureAccount = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE,
        domain->classMaximum(ResourceClass::RCK_WRITE));
    ResourceLease pressure(pressureAccount);
    pressure.set(domain->classMaximum(ResourceClass::RCK_WRITE) - baseline);
    const auto generation = backend.currentNativeMetadataGeneration(); backend.resetIoStats();
    EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndex({"write_bound", "Memory", {"value"}})),
        ResourceAdmissionDenied);
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    EXPECT_EQ(pressureAccount->used(), pressure.bytes());
    pressure.reset();
    {
        const auto created = backend.createLogicalNodeIndex({"write_bound", "Memory", {"value"}});
        EXPECT_NE(created.index_id, 0U);
        EXPECT_GT(domain->usage().classes[writeKind], baseline);
        EXPECT_EQ(created.name, "write_bound");
    }
    EXPECT_EQ(domain->usage().classes[writeKind], baseline);
}

TEST_F(RelocatableStorageBackendTest, SchemaBatchNameValidationRejectsWithoutWritingAndPreservesDefaultNames) {
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto generation = backend.currentNativeMetadataGeneration();
    const auto reject = [&](std::vector<LogicalGraphNodeIndexCreateRequest> requests) {
        backend.resetIoStats();
        EXPECT_THROW(static_cast<void>(backend.createLogicalNodeIndexes(requests)), std::invalid_argument);
        EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
        EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    };
    reject({{"same", "Memory", {"value"}}, {"same", "Other", {"value"}}});
    reject({{"first", "Memory", {"value"}}, {"second", "Memory", {"value"}}});
    reject({{"repeat", "Memory", {"value", "value"}}});
    reject({{"", std::string(4096U, 'l'), {"value"}}});
    reject({{std::string(4097U, 'n'), "Memory", {"value"}}});
    EXPECT_THROW((void)backend.createLogicalNodeVectorIndex({"vector", std::string(4097U, 'l'), "value", 2U, "l2"}), FormatException);
    EXPECT_THROW((void)backend.createLogicalNodeVectorIndex({std::string(4097U, 'n'), "Memory", "value", 2U, "l2"}), FormatException);
    EXPECT_THROW((void)backend.createLogicalConstraint({"constraint", LogicalGraphConstraintEntityKind::LGCEK_NODE,
        LogicalGraphConstraintKind::LGCK_UNIQUE, std::string(4097U, 'l'), {"value"}}), FormatException);
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_EQ(backend.ioStats().data.write_calls, 0U);
    const auto created = backend.createLogicalNodeIndex({"", "Memory", {"value"}});
    EXPECT_EQ(created.name, "index_node_6_Memory_5_value");
}

TEST_F(RelocatableStorageBackendTest, EverySchemaDdlCheckpointCancellationRollsBackAndRefundsBeforeRetry) {
    struct Cancelled {};
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    std::vector<LogicalGraphNodeCreateRequest> historical;
    for (int64_t value = 0; value < 4; ++value)
        historical.push_back({{"CancelSchema"}, {{"value", PropertyValue(value)},
            {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(double(value + 1)), PropertyValue(2.0)})}}});
    const auto nodes = backend.createLogicalNodes(historical);
    const auto domain = backend.queryResourceDomain();
    const auto writeKind = static_cast<size_t>(execution::detail::ResourceClass::RCK_WRITE);
    const std::vector<LogicalGraphNodeIndexCreateRequest> requests{{"cancel_schema", "CancelSchema", {"value"}}};
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        const auto create = [&](const std::function<void()> &checkpoint) {
            if (mode == 0) (void)backend.createLogicalNodeIndexesWithCheckpoint(requests, checkpoint, {});
            else if (mode == 1) (void)backend.createLogicalNodeVectorIndexWithCheckpoint(
                {"cancel_schema", "CancelSchema", "embedding", 2U, "l2"}, checkpoint, {});
            else (void)backend.createLogicalConstraintWithCheckpoint({"cancel_schema",
                LogicalGraphConstraintEntityKind::LGCEK_NODE, LogicalGraphConstraintKind::LGCK_UNIQUE,
                "CancelSchema", {"value"}}, checkpoint, {});
        };
        const auto drop = [&] {
            if (mode == 2) EXPECT_TRUE(backend.dropLogicalConstraintByName("cancel_schema"));
            else EXPECT_TRUE(backend.dropLogicalIndexByName("cancel_schema"));
        };
        size_t checkpoints = 0U;
        create([&] { ++checkpoints; }); drop();
        ASSERT_GT(checkpoints, 0U);
        for (size_t cancelled = 0; cancelled < checkpoints; ++cancelled) {
            SCOPED_TRACE(cancelled);
            const auto baseline = domain->usage().classes[writeKind];
            const auto fence = backend.currentNativeLogicalGraphMutationFence();
            size_t visited = 0U;
            EXPECT_THROW(create([&] { if (visited++ == cancelled) throw Cancelled{}; }), Cancelled);
            EXPECT_EQ(visited, cancelled + 1U);
            EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
            EXPECT_EQ(backend.currentNativeLogicalGraphMutationFence(), fence);
            EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
            EXPECT_TRUE(backend.listLogicalConstraints(8U).empty());
            EXPECT_EQ(domain->usage().classes[writeKind], baseline);
            {
                auto snapshot = backend.openLogicalGraphReadSnapshot();
                for (size_t i = 0; i < nodes.size(); ++i)
                    EXPECT_EQ(snapshot->readNodeProperty(nodes[i], "value"), PropertyValue(static_cast<int64_t>(i)));
            }
            create({}); drop();
        }
    }
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    backend.close(); backend.open();
    EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
    EXPECT_TRUE(backend.listLogicalConstraints(8U).empty());
}

TEST_F(RelocatableStorageBackendTest, GdsProjectionsAndLazyCsrKeepCacheOwnershipWithoutRetainingTheirCreatingQuery) {
    using namespace graph::query::execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    static_cast<void>(backend->createLogicalNodes({{{"GdsOwner"}, {}}, {{"GdsOwner"}, {}}, {{"GdsOwner"}, {}}}));
    auto domain = backend->queryResourceDomain();
    graph::query::QueryEngine engine(backend);
    const auto queryClass = static_cast<size_t>(ResourceClass::RCK_QUERY);
    {
        const auto result = engine.execute("CALL gds.graph.project('owned', {})");
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_GT(result.retainedResourceBytes(), 0U);
        EXPECT_EQ(domain->usage().classes[queryClass], result.retainedResourceBytes());
    }
    EXPECT_EQ(domain->usage().classes[queryClass], 0U);
    {
        const auto result = engine.execute("CALL gds.pageRank.stream('owned')");
        EXPECT_EQ(result.rowCount(), 3U);
        EXPECT_GT(result.retainedResourceBytes(), 0U);
        EXPECT_EQ(domain->usage().classes[queryClass], result.retainedResourceBytes());
    }
    EXPECT_EQ(domain->usage().classes[queryClass], 0U);
    engine.resetEphemeralState(); backend->close();
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 0U);
}

TEST_F(RelocatableStorageBackendTest, ProcedureResultKeepsItsQueryCreditAfterBackendCloseAndRefundsOnDestruction) {
    using namespace graph::query::execution::detail;
    materializeNativeDatabase();
    auto backend = std::make_shared<RelocatableStorageBackend>(backendOptions()); backend->open();
    graph::query::QueryEngine engine(backend);
    auto domain = backend->queryResourceDomain();
    auto result = engine.execute("CALL gds.graph.exists('missing')");
    ASSERT_EQ(result.rowCount(), 1U); ASSERT_GT(result.retainedResourceBytes(), 0U);
    backend->close(); engine.resetEphemeralState();
    EXPECT_EQ(result.getRows().front().at("exists").asPrimitive(), PropertyValue(false));
    EXPECT_EQ(domain->usage().total, result.retainedResourceBytes());
    result = graph::query::QueryResult{};
    EXPECT_EQ(domain->usage().total, 0U);
}

TEST_F(RelocatableStorageBackendTest, NativeCommitDoesNotBorrowExhaustedCacheCreditForPrivateBtreeValidation) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"WritePressure"}, {{"value", PropertyValue(1)}}}});
    const auto domain = backend.queryResourceDomain();
    const auto write = static_cast<size_t>(ResourceClass::RCK_WRITE);
    const auto baseline = domain->usage().classes[write];
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(domain);
    std::vector<uint64_t> created;
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        ASSERT_EQ(cache->used(), cache->capacity());
        created = backend.createLogicalNodes({{{"WritePressure"}, {{"value", PropertyValue(2)}}}});
        ASSERT_EQ(created.size(), 1U);
        // Publication may invalidate old cached pages; private work must not require extra CACHE credit.
        EXPECT_LE(cache->used(), cache->capacity());
        EXPECT_EQ(domain->usage().classes[write], baseline);
    }
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->readNode(created.front()).properties.front().value, PropertyValue(2));
    snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeCowWriteAdmissionRejectsBeforePublicationAndRefundsForRetry) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    (void)backend.createLogicalNodes({{{"WritePressure"}, {{"value", PropertyValue(1)}}}});
    const auto domain = backend.queryResourceDomain();
    const auto kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    const auto baseline = domain->usage().classes[kind];
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, domain->classMaximum(ResourceClass::RCK_WRITE));
    ResourceLease pressure(account); pressure.set(account->available());
    const auto generation = backend.currentNativeMetadataGeneration(); backend.resetIoStats();
    EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"WritePressure"}, {{"value", PropertyValue(2)}}}})), ResourceAdmissionDenied);
    EXPECT_EQ(backend.ioStats().data.write_bytes, 0U);
    EXPECT_EQ(backend.currentNativeMetadataGeneration(), generation);
    EXPECT_FALSE(backend.hasIndeterminateWriteOutcome());
    pressure.reset();
    const auto created = backend.createLogicalNodes({{{"WritePressure"}, {{"value", PropertyValue(2)}}}});
    EXPECT_EQ(created.size(), 1U); EXPECT_EQ(domain->usage().classes[kind], baseline);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeEdgeOverlayResolvesAnUnseenTypeUnderExhaustedCacheCredit) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Endpoints"}, {}}, {{"Endpoints"}, {}}});
    ASSERT_EQ(nodes.size(), 2U);
    const auto domain = backend.queryResourceDomain();
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(domain);
    std::vector<uint64_t> edges;
    {
        auto transaction = backend.beginLogicalGraphTransaction(false);
        LogicalGraphTransactionOverlay overlay(*transaction);
        {
            PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
            ASSERT_EQ(cache->used(), cache->capacity());
            edges = overlay.createLogicalEdges({{nodes[0], nodes[1], "UnseenPressureType", {{"weight", PropertyValue(7)}}}});
            const auto committed = transaction->commit();
            EXPECT_EQ(committed.created_edge_ids, edges);
            EXPECT_LE(cache->used(), cache->capacity());
        }
    }
    auto snapshot = backend.openLogicalGraphReadSnapshot(); ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(edges.size(), 1U);
    const auto edge = snapshot->readEdge(edges.front());
    EXPECT_EQ(edge.type, "UnseenPressureType");
    EXPECT_EQ(edge.source_node_id, nodes[0]); EXPECT_EQ(edge.target_node_id, nodes[1]);
    EXPECT_EQ(edge.properties.front().value, PropertyValue(7));
    snapshot.reset(); backend.close();
}

TEST_F(RelocatableStorageBackendTest, NativeCowWorkspaceUsesExistingWriteAllowanceWithoutRaisingClassLimit) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto domain = backend.queryResourceDomain();
    ASSERT_EQ(domain->classMaximum(ResourceClass::RCK_WRITE), 128U * 1024U * 1024U);
    const auto baseline = domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)];
    struct RestoreTrace {
        bool previous = debug::PerfTrace::areValuesEnabled();
        RestoreTrace() { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
        ~RestoreTrace() { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(previous); }
    } restore;
    ASSERT_EQ(backend.createLogicalNodes({{{"WorkspacePolicy"}, {}}}).size(), 1U);
    const auto trace = debug::PerfTrace::snapshotAndReset();
    const auto grant = trace.find("native_graph.write.private_workspace_grant_bytes");
    ASSERT_NE(grant, trace.end());
    EXPECT_GT(grant->second.totalValue, 32U * 1024U * 1024U);
    EXPECT_LE(grant->second.totalValue, domain->classMaximum(ResourceClass::RCK_WRITE) - 4096U);
    const auto peak = trace.find("native_graph.write.private_workspace_peak_bytes");
    ASSERT_NE(peak, trace.end());
    EXPECT_LE(peak->second.totalValue, grant->second.totalValue);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], baseline);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, ConcurrentNativeWritesDoNotReserveCreditWhileWaitingForAnotherPublication) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto domain = backend.queryResourceDomain();
    const auto kind = static_cast<size_t>(ResourceClass::RCK_WRITE);
    const auto baseline = domain->usage().classes[kind];
    std::barrier start(4);
    std::array<std::exception_ptr,4> errors{};
    std::array<uint64_t,4> ids{};
    std::vector<std::jthread> writers;
    for (size_t index=0;index<4;++index) writers.emplace_back([&,index] {
        start.arrive_and_wait();
        try { ids[index] = backend.createLogicalNodes({{{"ConcurrentCredit"}, {{"value",PropertyValue(int64_t(index))}}}}).at(0); }
        catch (...) { errors[index]=std::current_exception(); }
    });
    for (auto &writer:writers) writer.join();
    for (const auto &error:errors) {
        if (error) { try { std::rethrow_exception(error); } catch (const std::exception &failure) { ADD_FAILURE()<<failure.what(); } }
    }
    auto snapshot=backend.openLogicalGraphReadSnapshot();
    for (size_t index=0;index<4;++index) {
        ASSERT_NE(ids[index],0U);
        EXPECT_EQ(snapshot->readNode(ids[index]).properties.front().value,PropertyValue(int64_t(index)));
    }
    snapshot.reset();
    EXPECT_EQ(domain->usage().classes[kind],baseline);
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, OverlayEndpointValidationDoesNotBorrowExhaustedCacheCredit) {
 using namespace execution::detail;
 materializeNativeDatabase();
 auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
 RelocatableStorageBackend backend(options); backend.open();
 const auto nodes = backend.createLogicalNodes({{{"Endpoints"}, {}}, {{"Endpoints"}, {}}});
 ASSERT_EQ(nodes.size(), 2U);
 const auto domain = backend.queryResourceDomain();
 auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(domain);
 {
  auto transaction = backend.beginLogicalGraphTransaction(false);
  LogicalGraphTransactionOverlay overlay(*transaction);
  auto snapshot = overlay.openLogicalGraphReadSnapshot();
  const auto baseline = domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)];
  PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
  ASSERT_EQ(cache->used(), cache->capacity());
  ASSERT_NO_THROW(EXPECT_TRUE(snapshot->containsNode(nodes.front())));
  ASSERT_NO_THROW(EXPECT_FALSE(snapshot->containsNode(nodes.back() + 100U)));
  const std::array<uint64_t, 3U> probes{nodes.front(), nodes.back(), nodes.back() + 100U};
  std::vector<bool> observed;
  ASSERT_NO_THROW(observed = snapshot->containsNodesSorted(probes));
  EXPECT_EQ(observed, (std::vector<bool>{true, true, false}));
  EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], baseline);
  EXPECT_LE(cache->used(), cache->capacity());
 }
 backend.close();
}


TEST_F(RelocatableStorageBackendTest, BoundPropertyTokenResolutionDoesNotBorrowExhaustedCacheCredit) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    (void)backend->createLogicalNodes({{{"BoundAggregate"}, {{"group", PropertyValue(7)}}}});
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(backend->queryResourceDomain());
    auto account = std::make_shared<ResourceAccount>(backend->queryResourceDomain(), ResourceClass::RCK_QUERY, 65536U);
    {
        execution::MaterializedByteBudget budget(65536U, account);
        BoundSlotRegistry slots(1U, 1U, 32U, budget);
        const auto slot = slots.declare(slots.root(), "n"); slots.seal();
        const std::array<BoundPropertyRequest, 1U> requests{{{slot, "group"}}};
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        BoundPropertyBindings bindings(slots, *snapshot, requests, 1U, 32U, budget);
        EXPECT_TRUE(bindings.view(bindings.descriptor(0U), *snapshot).token.has_value());
    }
    EXPECT_EQ(account->used(), 0U);
    snapshot.reset(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, FullNativeReadKeepsPrivateSnapshotAndMetadataOutOfExhaustedCache) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    (void)backend->createLogicalNodes({{{"CacheFullAggregate"}, {{"group", PropertyValue(7)}}},
                                      {{"CacheFullAggregate"}, {{"group", PropertyValue(7)}}}});
    auto domain = backend->queryResourceDomain();
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(domain);
    query::QueryEngine engine(backend);
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        auto result = engine.execute("MATCH (n:CacheFullAggregate) RETURN n.group AS group,count(*) AS count");
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows().front().at("group").asPrimitive(), PropertyValue(7));
        EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(2));
        auto absent = engine.execute("MATCH ()-[r:MissingReadType]->() RETURN count(*) AS count");
        ASSERT_EQ(absent.rowCount(), 1U);
        EXPECT_EQ(absent.getRows().front().at("count").asPrimitive(), PropertyValue(0));
    }
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_QUERY)], 0U);
    engine.resetEphemeralState(); backend->close();
}

TEST_F(RelocatableStorageBackendTest, WritableReopenUsesProtectedMaintenanceScratchUnderExhaustedCache) {
    using namespace execution::detail;
    materializeNativeDatabase();
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
    (void)backend->createLogicalNodes({{{"RecoveryPressure"}, {{"value", PropertyValue(7)}}}});
    auto domain = backend->queryResourceDomain();
    auto cache = ProcessResourceDomains::defaultOwner()->cacheGrant(domain);
    backend->close();
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        backend->open();
        query::QueryEngine engine(backend);
        auto result = engine.execute("MATCH (n:RecoveryPressure) RETURN n.value AS value");
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(7));
    }
    backend->close();
}
