#include <gtest/gtest.h>
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::query {
namespace {

		class IdentityOnlyLogicalGraphReadBackend final : public storage::LogicalGraphReadBackend {
		public:
			std::optional<storage::LogicalGraphSchemaIdentity> identity{storage::LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 0U}};
			mutable size_t observations = 0U;
            std::shared_ptr<std::pmr::memory_resource> cache_memory;
            std::shared_ptr<execution::detail::DatabaseResourceBudget> domain;
            std::shared_ptr<execution::detail::DatabaseResourceBudget> queryResourceDomain() const override { return domain; }
            std::shared_ptr<std::pmr::memory_resource> logicalPlanCacheMemoryOwner() const override { return cache_memory; }
			std::optional<storage::LogicalGraphSchemaIdentity> logicalPlanCacheIdentity() const override {
				++observations; return identity;
			}
			std::unique_ptr<storage::LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				throw std::logic_error("cache identity must not open a logical snapshot");
			}
		};
}

	TEST(QueryEngineLogicalBackendBoundaryTest, InvalidatesLogicalCacheOnIdentityOrCapabilityChangeWithoutOpeningLogicalSnapshots) {
		auto backend = std::make_shared<IdentityOnlyLogicalGraphReadBackend>(); QueryEngine engine(backend);
		for (int variant = 0; variant < 5; ++variant) {
			engine.buildPlan("RETURN 1"); engine.buildPlan("RETURN 2"); ASSERT_EQ(engine.getPlanCache().size(), 2U);
			if (variant == 0) ++backend->identity->database_token;
			if (variant == 1) ++backend->identity->format_token;
			if (variant == 2) ++backend->identity->catalog_tokens[0];
			if (variant == 3) backend->identity.reset();
			if (variant == 4) backend->identity = storage::LogicalGraphSchemaIdentity{9U, 2U, {}, 0U, 0U};
			engine.buildPlan("RETURN 1"); EXPECT_EQ(engine.getPlanCache().size(), 1U);
		}
		const auto observations = backend->observations;
		engine.buildPlan("EXPLAIN MATCH (n) RETURN n");
		EXPECT_EQ(backend->observations, observations);
		engine.resetEphemeralState(); EXPECT_EQ(engine.getPlanCache().size(), 0U);
	}
    TEST(QueryEngineLogicalBackendBoundaryTest, RebindsCacheOwnershipWithoutOpeningLogicalSnapshots) {
        auto backend = std::make_shared<IdentityOnlyLogicalGraphReadBackend>();
        auto first = std::make_shared<execution::MaterializedByteBudget>(65536U);
        auto second = std::make_shared<execution::MaterializedByteBudget>(65536U);
        backend->cache_memory = first;
        QueryEngine engine(backend);
        auto plan = engine.buildPlan("RETURN 1");
        ASSERT_GT(first->used(), 0U);
        backend->cache_memory = second;
        auto next = engine.buildPlan("RETURN 1");
        EXPECT_EQ(first->used(), 0U);
        EXPECT_GT(second->used(), 0U);
        EXPECT_EQ(engine.getPlanCache().size(), 1U);
        engine.resetEphemeralState();
        EXPECT_EQ(second->used(), 0U);
        EXPECT_TRUE(plan.root); EXPECT_TRUE(next.root);
    }

    TEST(QueryEngineLogicalBackendBoundaryTest, SharedCachePressureDoesNotRejectAnOtherwiseAdmittedCompilation) {
        using namespace execution::detail;
        auto backend = std::make_shared<IdentityOnlyLogicalGraphReadBackend>();
        auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1024U * 1024U));
        backend->domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(1024U * 1024U));
        auto cacheMemory = std::make_shared<execution::MaterializedByteBudget>(65536U);
        backend->cache_memory = cacheMemory;
        QueryEngine engine(backend);
        { auto warm = engine.buildPlan("RETURN 1"); }
        ASSERT_EQ(engine.getPlanCache().size(), 1U);
        {
            utils::OwnedHeapReservation pressure(cacheMemory, cacheMemory->available());
            auto plan = engine.buildPlan("RETURN 2");
            ASSERT_TRUE(plan.root);
            EXPECT_EQ(engine.getPlanCache().size(), 1U);
            plan.root.reset();
            EXPECT_EQ(backend->domain->usage().total, 0U);
        }
        { auto retry = engine.buildPlan("RETURN 2"); ASSERT_TRUE(retry.root); }
        EXPECT_EQ(engine.getPlanCache().size(), 2U);
        engine.resetEphemeralState();
        EXPECT_EQ(cacheMemory->used(), 0U);
    }

    TEST(QueryEngineLogicalBackendBoundaryTest, ManagedBackendsWithoutSharedCacheAuthorityDoNotRetainPrivateQueryAccounts) {
        using namespace execution::detail;
        auto backend = std::make_shared<IdentityOnlyLogicalGraphReadBackend>();
        auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1024U * 1024U));
        backend->domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(1024U * 1024U));
        QueryEngine engine(backend);
        for (size_t index = 0U; index < 96U; ++index) {
            { auto plan = engine.buildPlan("RETURN " + std::to_string(index)); ASSERT_TRUE(plan.root); }
            EXPECT_EQ(backend->domain->usage().total, 0U);
            EXPECT_EQ(engine.getPlanCache().size(), 0U);
        }
    }

}
