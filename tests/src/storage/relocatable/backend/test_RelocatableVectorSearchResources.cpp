/**
 * @brief Vector search allocation authority, rejection and escaped-result contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/api/QueryEngine.hpp"

namespace {
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
class SearchFaultBudget final : public graph::utils::UnmanagedAllocationResource {
public:
    std::shared_ptr<MaterializedByteBudget> memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    size_t requests = 0U, fault = (std::numeric_limits<size_t>::max)();
    bool triggered = false;
    void reserveUnmanaged(size_t bytes) override { attempt(); memory->reserveUnmanaged(bytes); }
    void releaseUnmanaged(size_t bytes) noexcept override { memory->releaseUnmanaged(bytes); }
private:
    void attempt() { if (requests++ == fault) { triggered = true; throw MaterializedByteLimitExceeded(); } }
    void *do_allocate(size_t bytes, size_t alignment) override { attempt(); return memory->allocate(bytes, alignment); }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override { memory->deallocate(pointer, bytes, alignment); }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(LogicalVectorResultResourcesTest, HitCopyAndMoveRetainTheAllocationAuthorityAndRejectAnUnfundedClone) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    std::optional<LogicalGraphVectorSearchResult> escaped;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        escaped.emplace(); escaped->hits.reserve(17U); escaped->hits.push_back({7U, 0.5F});
        EXPECT_EQ(memory->used(), escaped->hits.retainedBackingRequestBytes());
        const auto before = memory->used();
        auto denied = std::make_shared<MaterializedByteBudget>(1U);
        {
            graph::utils::OwnedAllocationScope other(denied);
            EXPECT_THROW(static_cast<void>(LogicalGraphVectorSearchResult(*escaped)), MaterializedByteLimitExceeded);
        }
        EXPECT_EQ(denied->used(), 0U); EXPECT_EQ(memory->used(), before);
        EXPECT_EQ(escaped->hits.front(), (LogicalGraphVectorSearchHit{7U, 0.5F}));
        auto clone = *escaped;
        EXPECT_GT(memory->used(), before);
        auto moved = std::move(clone);
        EXPECT_EQ(moved.hits, escaped->hits);
        clone.hits.push_back({8U, 1.0F});
        EXPECT_EQ(clone.hits.size(), 1U);
    }
    memory.reset(); EXPECT_FALSE(authority.expired());
    escaped.reset(); EXPECT_TRUE(authority.expired());
}

TEST_F(RelocatableStorageBackendSchemaTest, NativeExactVectorSearchRefundsEveryAllocationFailureAndRetainsEscapedHits) {
    RelocatableStorageBackend backend(options()); backend.open();
    const auto vector = [](double x, double y) {
        return graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(x), graph::PropertyValue(y)});
    };
    const auto nodes = backend.createLogicalNodes({
        {{"Person"}, {{"embedding", vector(1.0, -2.0)}}},
        {{"Person"}, {{"embedding", vector(-3.0, 4.0)}}}});
    static_cast<void>(backend.createLogicalNodeVectorIndex({"owned_vector", "Person", "embedding", 2U, "inner_product"}));
    auto snapshot = backend.openLogicalGraphVectorSearchSnapshot(); ASSERT_NE(snapshot, nullptr);
    LogicalGraphVectorSearchRequest request;
    request.index_name = "owned_vector"; request.query_vector = {1.0F, -2.0F}; request.top_k = 2U;
    request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
    request.maximum_mapping_entries = 2U; request.maximum_raw_vector_reads = 2U;
    static_cast<void>(snapshot->searchLogicalVector(request)); // Warm independent CACHE state.
    auto observed = std::make_shared<SearchFaultBudget>();
    {
        graph::utils::OwnedAllocationScope scope(observed);
        const auto result = snapshot->searchLogicalVector(request);
        ASSERT_EQ(result.hits.size(), 2U);
        EXPECT_EQ(result.hits[0].node_id, nodes[0]); EXPECT_EQ(result.hits[1].node_id, nodes[1]);
        EXPECT_EQ(observed->memory->used(), result.hits.retainedBackingRequestBytes());
    }
    ASSERT_EQ(observed->memory->used(), 0U);
    const auto requests = observed->requests; ASSERT_GT(requests, 1U);
    for (size_t fault = 0U; fault < requests; ++fault) {
        SCOPED_TRACE(fault);
        auto denied = std::make_shared<SearchFaultBudget>(); denied->fault = fault;
        {
            graph::utils::OwnedAllocationScope scope(denied);
            EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), MaterializedByteLimitExceeded);
        }
        EXPECT_TRUE(denied->triggered); EXPECT_EQ(denied->memory->used(), 0U);
    }
    RecordProperty("native_exact_admission_points", std::to_string(requests));
    std::optional<LogicalGraphVectorSearchResult> escaped;
    std::weak_ptr<std::pmr::memory_resource> authority = observed;
    {
        graph::utils::OwnedAllocationScope scope(observed);
        escaped.emplace(snapshot->searchLogicalVector(request));
    }
    snapshot.reset(); backend.close(); observed.reset();
    EXPECT_FALSE(authority.expired());
    ASSERT_EQ(escaped->hits.size(), 2U); EXPECT_EQ(escaped->hits[0].node_id, nodes[0]);
    escaped.reset(); EXPECT_TRUE(authority.expired());
}

TEST_F(RelocatableStorageBackendSchemaTest, ProcedurePayloadsPublishExactOwnershipAndRefundQuotaFailures) {
    using namespace graph::query::execution::detail;
    auto backend = std::make_shared<RelocatableStorageBackend>(options()); backend->open();
    const auto vector = [](double x, double y) {
        return graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(x), graph::PropertyValue(y)});
    };
    static_cast<void>(backend->createLogicalNodes({
        {{"Person"}, {{"embedding", vector(1.0, -2.0)}, {"payload", graph::PropertyValue(std::string(2000U, 'a'))}}},
        {{"Person"}, {{"embedding", vector(-3.0, 4.0)}, {"payload", graph::PropertyValue(std::string(2000U, 'b'))}}}}));
    static_cast<void>(backend->createLogicalNodeVectorIndex({"owned_vector", "Person", "embedding", 2U, "inner_product"}));
    graph::query::QueryEngine engine(backend, backend);
    const std::string prefix = "CALL db.index.vector.queryNodes('owned_vector', 2, [1.0, -2.0]) YIELD node, score ";
    const std::vector<std::pair<std::string, size_t>> cases{
        {"RETURN node.payload AS value", 2U},
        {"WITH node AS bound RETURN bound AS value", 2U},
        {"RETURN DISTINCT node.payload AS value", 2U},
        {"WITH node, score ORDER BY score DESC LIMIT 1 RETURN node.payload AS value", 1U},
        {"RETURN labels(node) AS value", 2U},
        {"RETURN properties(node) AS value", 2U},
        {"WITH node.payload AS value RETURN value", 2U}};
    const auto domain = backend->queryResourceDomain();
    for (const auto &[suffix, count] : cases) {
        SCOPED_TRACE(suffix);
        graph::query::QueryContext context;
        auto denied = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 1024U);
        context.resource_account = denied;
        EXPECT_THROW(static_cast<void>(engine.execute(prefix + suffix, context)), ResourceAdmissionDenied);
        EXPECT_EQ(denied->used(), 0U);
        auto admitted = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 256U * 1024U);
        context.resource_account = admitted;
        {
            auto result = engine.execute(prefix + suffix, context);
            ASSERT_EQ(result.rowCount(), count);
            EXPECT_EQ(admitted->used(), result.retainedResourceBytes());
            ASSERT_TRUE(result.getRows().front().contains("value"));
            if (suffix == "RETURN node.payload AS value" || suffix == "WITH node.payload AS value RETURN value")
                EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), graph::PropertyValue(std::string(2000U, 'a')));
            if (suffix == "WITH node AS bound RETURN bound AS value")
                EXPECT_EQ(result.getRows().front().at("value").asLogicalNode().properties.at("payload"), graph::PropertyValue(std::string(2000U, 'a')));
        }
        EXPECT_EQ(admitted->used(), 0U);
    }
    engine.resetEphemeralState(); backend->close();
    EXPECT_EQ(domain->usage().total, 0U);
}
