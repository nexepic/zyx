/**
 * @file test_LogicalGdsResourceOwnership.cpp
 * @brief Projection and lazy-CSR allocation ownership and rejection contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <optional>
#include "query/api/LogicalProcedureTestFixture.hpp"
#include "graph/query/gds/LogicalGraphProjection.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::gds {
namespace {
using execution::MaterializedByteBudget;
using execution::MaterializedByteLimitExceeded;
class LogicalGdsResourceOwnershipTest : public ::testing::Test {
protected:
    test::RecordingVectorProcedureBackend backend;
    LogicalGraphProjectionSpec spec{.name = "owned"};
    LogicalProjectionBuildBounds bounds{.maximum_nodes = 16U, .maximum_metadata_pages = 16U,
        .maximum_edges_per_node = 16U};
    void SetUp() override {
        for (uint64_t id = 1U; id <= 4U; ++id)
            backend.state->nodes.emplace(id, storage::LogicalGraphNode{id, {}});
        backend.state->edges = {{11U, 1U, 2U, "LINK"}, {12U, 2U, 3U, "LINK"}, {13U, 1U, 4U, "LINK"}};
    }
    LogicalGraphProjection build() {
        auto snapshot = backend.openLogicalGraphReadSnapshot();
        return LogicalGraphProjection::build(*snapshot, spec, bounds);
    }
};

TEST_F(LogicalGdsResourceOwnershipTest, ProjectionRejectsBeforeScanningAndRefundsItsPartialConstruction) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U);
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(build()), MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(backend.state->topology_scans, 0U);
    auto admitted = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    {
        utils::OwnedAllocationScope scope(admitted);
        const auto projection = build();
        EXPECT_EQ(projection.nodeCount(), 4U); EXPECT_EQ(projection.edgeCount(), 3U);
    }
    EXPECT_EQ(admitted->used(), 0U);
}

TEST_F(LogicalGdsResourceOwnershipTest, EscapedLazyCsrUsesTheOriginatingAuthorityAndOutlivesTheProjection) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    std::optional<LogicalGraphProjection> projection;
    {
        utils::OwnedAllocationScope scope(memory);
        projection.emplace(build());
    }
    const auto before = memory->used();
    EXPECT_GT(before, 0U);
    auto other = std::make_shared<MaterializedByteBudget>(1U);
    std::shared_ptr<const LogicalProjectionCsr> csr;
    {
        utils::OwnedAllocationScope scope(other);
        ASSERT_NO_THROW(csr = projection->csr());
    }
    EXPECT_EQ(other->used(), 0U);
    EXPECT_GT(memory->used(), before);
    ASSERT_EQ(csr->node_ids, std::vector<uint64_t>({1U, 2U, 3U, 4U}));
    EXPECT_EQ(csr->edgeCount(), 3U);
    const std::vector<LogicalProjectionEdge> &legacyView = projection->outgoing(1U);
    EXPECT_EQ(legacyView.size(), 2U);
    memory.reset(); projection.reset();
    EXPECT_FALSE(authority.expired());
    csr.reset(); EXPECT_TRUE(authority.expired());
}

TEST_F(LogicalGdsResourceOwnershipTest, DeniedLazyCsrPublishesNothingAndRefundsBeforeTheSameProjectionRetries) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::optional<LogicalGraphProjection> projection;
    {
        utils::OwnedAllocationScope scope(memory);
        projection.emplace(build());
    }
    const auto retained = memory->used();
    execution::MaterializedByteReservation pressure(memory.get());
    pressure.set(memory->available() - 1U);
    const auto exhausted = memory->used();
    EXPECT_THROW(static_cast<void>(projection->csr()), MaterializedByteLimitExceeded);
    EXPECT_EQ(memory->used(), exhausted);
    EXPECT_FALSE(projection->csrCacheState().materialized);
    pressure.reset(); EXPECT_EQ(memory->used(), retained);
    const auto csr = projection->csr();
    EXPECT_TRUE(projection->csrCacheState().materialized);
    EXPECT_EQ(projection->csr(), csr);
    EXPECT_EQ(csr->edgeCount(), 3U);
}

TEST_F(LogicalGdsResourceOwnershipTest, DeniedProjectionCopyPreservesTheSourceAndItsCompleteAdjacency) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::optional<LogicalGraphProjection> projection;
    {
        utils::OwnedAllocationScope scope(memory);
        projection.emplace(build());
    }
    const auto before = memory->used();
    auto denied = std::make_shared<MaterializedByteBudget>(1U);
    {
        utils::OwnedAllocationScope scope(denied);
        EXPECT_THROW({ const auto copy = *projection; }, MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(denied->used(), 0U); EXPECT_EQ(memory->used(), before);
    EXPECT_EQ(projection->nodeCount(), 4U); EXPECT_EQ(projection->edgeCount(), 3U);
    EXPECT_EQ(projection->outgoing(1U).size(), 2U); EXPECT_EQ(projection->incoming(3U).size(), 1U);
}
TEST_F(LogicalGdsResourceOwnershipTest, LegacyProjectionDoesNotBorrowAnUnrelatedDefaultPmrResource) {
    class RejectAllocations final : public std::pmr::memory_resource {
        void *do_allocate(size_t, size_t) override { throw std::bad_alloc(); }
        void do_deallocate(void *, size_t, size_t) override { std::terminate(); }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    } rejected;
    struct RestoreDefault {
        std::pmr::memory_resource *previous;
        ~RestoreDefault() { std::pmr::set_default_resource(previous); }
    };
    std::optional<LogicalGraphProjection> projection;
    {
        RestoreDefault restore{std::pmr::set_default_resource(&rejected)};
        ASSERT_NO_THROW(projection.emplace(build()));
    }
    ASSERT_EQ(projection->edgeCount(), 3U);
    EXPECT_EQ(projection->csr()->edgeCount(), 3U);
}
TEST_F(LogicalGdsResourceOwnershipTest, CatalogCommitAndLazyCsrKeepTheirCacheOwnerUnderAForeignCaller) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    auto other = std::make_shared<MaterializedByteBudget>(1U);
    LogicalProjectionCatalog catalog;
    std::optional<LogicalProjectionCatalogCreatePreparation> preparation;
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    {
        utils::OwnedAllocationScope scope(memory);
        preparation.emplace(catalog.prepareCreate(spec, *snapshot, bounds));
    }
    {
        utils::OwnedAllocationScope scope(other);
        ASSERT_NO_THROW(preparation->mutation->commit());
    }
    preparation.reset();
    EXPECT_EQ(other->used(), 0U);
    auto retained = catalog.get(spec.name);
    std::shared_ptr<const LogicalProjectionCsr> csr;
    {
        utils::OwnedAllocationScope scope(other);
        ASSERT_NO_THROW(csr = retained->csr());
    }
    EXPECT_EQ(other->used(), 0U);
    EXPECT_EQ(catalog.dropAll(), 1U);
    memory.reset(); retained.reset();
    EXPECT_FALSE(authority.expired());
    csr.reset(); EXPECT_TRUE(authority.expired());
}
TEST_F(LogicalGdsResourceOwnershipTest, AbandonedCatalogPreparationRefundsCandidateMetadataAndRevisionState) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    LogicalProjectionCatalog catalog;
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    {
        utils::OwnedAllocationScope scope(memory);
        const auto abandoned = catalog.prepareCreate(spec, *snapshot, bounds);
        EXPECT_GT(memory->used(), 0U);
    }
    EXPECT_FALSE(catalog.exists(spec.name));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(LogicalGdsResourceOwnershipTest, EveryPreparationAdmissionFailureRestoresTheEmptyCatalogAndRefundsBeforeRetry) {
    class FaultBudget final : public utils::UnmanagedAllocationResource {
    public:
        std::shared_ptr<MaterializedByteBudget> memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
        size_t requests = 0U, fault = (std::numeric_limits<size_t>::max)();
        bool triggered = false;
        void reserveUnmanaged(size_t bytes) override { attempt(); memory->reserveUnmanaged(bytes); }
        void releaseUnmanaged(size_t bytes) noexcept override { memory->releaseUnmanaged(bytes); }
    private:
        void attempt() {
            if (requests++ == fault) { triggered = true; throw MaterializedByteLimitExceeded(); }
        }
        void *do_allocate(size_t bytes, size_t alignment) override {
            attempt(); return memory->allocate(bytes, alignment);
        }
        void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
            memory->deallocate(pointer, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };
    spec.name = std::string(4096U, 'n');
    LogicalProjectionCatalog catalog;
    auto snapshot = backend.openLogicalGraphReadSnapshot();
    const auto prepare = [&] { const auto abandoned = catalog.prepareCreate(spec, *snapshot, bounds); };
    auto observed = std::make_shared<FaultBudget>();
    {
        utils::OwnedAllocationScope scope(observed);
        prepare();
    }
    ASSERT_GT(observed->requests, 0U);
    ASSERT_EQ(observed->memory->used(), 0U);
    const auto requests = observed->requests;
    for (size_t fault = 0U; fault < requests; ++fault) {
        SCOPED_TRACE(fault);
        auto denied = std::make_shared<FaultBudget>(); denied->fault = fault;
        {
            utils::OwnedAllocationScope scope(denied);
            EXPECT_THROW(prepare(), MaterializedByteLimitExceeded);
        }
        EXPECT_TRUE(denied->triggered);
        EXPECT_EQ(denied->memory->used(), 0U);
        EXPECT_FALSE(catalog.exists(spec.name));
    }
    {
        utils::OwnedAllocationScope scope(observed);
        prepare();
    }
    EXPECT_EQ(observed->memory->used(), 0U);
    RecordProperty("catalog_preparation_admission_points", std::to_string(requests));
}
TEST_F(LogicalGdsResourceOwnershipTest, EveryAlgorithmWorkspaceRejectsQueryPressureWithoutChargingTheProjectionCache) {
    auto cache = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::optional<LogicalGraphProjection> projection;
    {
        utils::OwnedAllocationScope scope(cache);
        projection.emplace(build()); static_cast<void>(projection->csr());
    }
    const auto before = cache->used();
    const LogicalGraphAlgorithmBounds limits{.maximum_nodes = 16U, .maximum_edges = 32U,
        .maximum_iterations = 64U, .maximum_work_units = 100000U, .maximum_materialized_bytes = 1024U * 1024U};
    auto query = std::make_shared<MaterializedByteBudget>(1U);
    {
        utils::OwnedAllocationScope scope(query);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRankOwned(*projection, {}, limits)), MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponentsOwned(*projection, limits)), MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::betweennessCentralityOwned(*projection, {}, limits)), MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::closenessCentralityOwned(*projection, limits)), MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenOwned(*projection, {}, limits)), MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(*projection, 1U, 3U, limits)), MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(query->used(), 0U); EXPECT_EQ(cache->used(), before);
}
TEST_F(LogicalGdsResourceOwnershipTest, OwnedAlgorithmResultsRetainOnlyTheirEscapedQueryArrays) {
    auto projection = build(); static_cast<void>(projection.csr());
    const LogicalGraphAlgorithmBounds limits{.maximum_nodes = 16U, .maximum_edges = 32U,
        .maximum_iterations = 64U, .maximum_work_units = 100000U, .maximum_materialized_bytes = 1024U * 1024U};
    const auto check = [&](auto produce) {
        auto query = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
        {
            utils::OwnedAllocationScope scope(query);
            const auto result = produce();
            EXPECT_EQ(result.size(), 4U);
            EXPECT_EQ(query->used(), result.retainedBackingRequestBytes());
        }
        EXPECT_EQ(query->used(), 0U);
    };
    check([&] { return LogicalGraphAlgorithms::pageRankOwned(projection, {}, limits); });
    check([&] { return LogicalGraphAlgorithms::weaklyConnectedComponentsOwned(projection, limits); });
    check([&] { return LogicalGraphAlgorithms::betweennessCentralityOwned(projection, {}, limits); });
    check([&] { return LogicalGraphAlgorithms::closenessCentralityOwned(projection, limits); });
    check([&] { return LogicalGraphAlgorithms::leidenOwned(projection, {}, limits); });
    auto query = std::make_shared<MaterializedByteBudget>(65536U);
    std::weak_ptr<std::pmr::memory_resource> authority = query;
    std::optional<LogicalGdsWeightedPath> path;
    {
        utils::OwnedAllocationScope scope(query);
        path.emplace(LogicalGraphAlgorithms::dijkstra(projection, 1U, 3U, limits));
        EXPECT_EQ(path->node_ids, std::vector<uint64_t>({1U, 2U, 3U}));
        EXPECT_EQ(query->used(), path->node_ids.retainedBackingRequestBytes() + path->step_weights.retainedBackingRequestBytes());
    }
    query.reset(); EXPECT_FALSE(authority.expired());
    path.reset(); EXPECT_TRUE(authority.expired());
}
TEST_F(LogicalGdsResourceOwnershipTest, AllAlgorithmAdmissionPointsRefundBeforeRetryAndLeaveTheCsrReusable) {
    class FaultBudget final : public utils::UnmanagedAllocationResource {
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
    auto projection = build(); const auto csr = projection.csr();
    const LogicalGraphAlgorithmBounds limits{.maximum_nodes = 16U, .maximum_edges = 32U,
        .maximum_iterations = 64U, .maximum_work_units = 100000U, .maximum_materialized_bytes = 1024U * 1024U};
    concurrent::ThreadPool pool(2U);
    const auto exercise = [&](int mode) {
        switch (mode) {
            case 0: static_cast<void>(LogicalGraphAlgorithms::pageRankOwned(projection, {}, limits)); break;
            case 1: static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponentsOwned(projection, limits)); break;
            case 2: static_cast<void>(LogicalGraphAlgorithms::betweennessCentralityOwned(projection, {}, limits)); break;
            case 3: static_cast<void>(LogicalGraphAlgorithms::closenessCentralityOwned(projection, limits)); break;
            case 4: static_cast<void>(LogicalGraphAlgorithms::leidenOwned(projection, {}, limits, {}, &pool)); break;
            case 5: static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 3U, limits)); break;
        }
    };
    size_t total = 0U;
    for (int mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode);
        auto observed = std::make_shared<FaultBudget>();
        { utils::OwnedAllocationScope scope(observed); exercise(mode); }
        ASSERT_EQ(observed->memory->used(), 0U);
        const auto requests = observed->requests;
        ASSERT_GT(requests, 0U); total += requests;
        for (size_t fault = 0U; fault < requests; ++fault) {
            SCOPED_TRACE(fault);
            auto denied = std::make_shared<FaultBudget>(); denied->fault = fault;
            { utils::OwnedAllocationScope scope(denied); EXPECT_THROW(exercise(mode), MaterializedByteLimitExceeded); }
            EXPECT_TRUE(denied->triggered); EXPECT_EQ(denied->memory->used(), 0U);
            EXPECT_EQ(projection.csr(), csr);
        }
        { utils::OwnedAllocationScope scope(observed); exercise(mode); }
        EXPECT_EQ(observed->memory->used(), 0U);
    }
    RecordProperty("algorithm_admission_points", std::to_string(total));
}
} // namespace
} // namespace graph::query::gds
