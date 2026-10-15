/**
 * @file test_PlanCache.cpp
 * @brief Focused tests for PlanCache update/hit/miss branches.
 */

#include <gtest/gtest.h>
#include <limits>
#include "graph/query/execution/PrepaidMemoryResource.hpp"

#include "graph/query/cache/PlanCache.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

using namespace graph::query;
using namespace graph::query::cache;
using namespace graph::query::logical;

TEST(PlanCacheTest, MissThenHitAndUpdateExistingEntry) {
	PlanCache cache(2);

	auto miss = cache.get("MATCH (n) RETURN n");
	EXPECT_FALSE(miss.has_value());
	EXPECT_EQ(cache.misses(), 1UL);

	QueryPlan plan;
	plan.root = std::make_unique<LogicalNodeScan>("n");
	cache.put("MATCH (n) RETURN n", plan);
	EXPECT_EQ(cache.size(), 1UL);

	auto hit = cache.get("MATCH (n) RETURN n");
	ASSERT_TRUE(hit.has_value());
	EXPECT_EQ(cache.hits(), 1UL);

	cache.put("MATCH (n) RETURN n", plan);
	EXPECT_EQ(cache.size(), 1UL);
}

TEST(PlanCacheTest, EvictsLeastRecentlyUsedWhenAtCapacity) {
	PlanCache cache(2);

	QueryPlan plan;
	plan.root = std::make_unique<LogicalNodeScan>("n");

	cache.put("q1", plan);
	cache.put("q2", plan);
	ASSERT_EQ(cache.size(), 2UL);

	(void)cache.get("q2");
	cache.put("q3", plan);

	EXPECT_EQ(cache.size(), 2UL);
	EXPECT_FALSE(cache.get("q1").has_value());
}

namespace {
class FailingCloneScan final : public LogicalNodeScan {
public:
	FailingCloneScan() : LogicalNodeScan("failing") {}
	std::unique_ptr<LogicalOperator> clone() const override { throw std::bad_alloc(); }
};
}

TEST(PlanCacheTest, ZeroCapacityDisablesCachingWithoutCloningOrEvictingAnEmptyList) {
	PlanCache cache(0U); QueryPlan plan; plan.root = std::make_unique<FailingCloneScan>();
	EXPECT_NO_THROW(cache.put("disabled", plan));
	EXPECT_EQ(cache.size(), 0U); EXPECT_FALSE(cache.get("disabled"));
}

TEST(PlanCacheTest, CloneFailureKeepsThePriorEntryAndItsEvictionOrder) {
	PlanCache cache(2U); QueryPlan good; good.root = std::make_unique<LogicalNodeScan>("good");
	QueryPlan failed; failed.root = std::make_unique<FailingCloneScan>();
	cache.put("oldest", good); cache.put("newest", good);
	EXPECT_THROW(cache.put("oldest", failed), std::bad_alloc);
	EXPECT_THROW(cache.put("new", failed), std::bad_alloc);
	EXPECT_EQ(cache.size(), 2U);
	cache.put("replacement", good);
	EXPECT_FALSE(cache.get("oldest"));
	ASSERT_TRUE(cache.get("newest"));
	EXPECT_EQ(cache.get("replacement")->root->getOutputVariables(), std::vector<std::string>{"good"});
}

namespace {
using namespace graph::query::execution::detail;
std::shared_ptr<PrepaidMemoryResource> planMetadataGrant(size_t bytes) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
	auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, bytes);
	return std::make_shared<PrepaidMemoryResource>(account, bytes);
}
class FailingPlanMetadata final : public std::pmr::memory_resource {
public:
	explicit FailingPlanMetadata(std::shared_ptr<PrepaidMemoryResource> grant) : grant(std::move(grant)) {}
	std::shared_ptr<PrepaidMemoryResource> grant;
	size_t calls = 0U, fail_at = (std::numeric_limits<size_t>::max)();
    size_t large_threshold = (std::numeric_limits<size_t>::max)(), large_requests = 0U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (++calls == fail_at) throw std::bad_alloc();
        if (bytes >= large_threshold) ++large_requests;
		return grant->allocate(bytes, alignment);
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override { grant->deallocate(pointer, bytes, alignment); }
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(PlanCacheTest, QueryTextAndMetadataCapacityCannotBypassItsAllocationGrant) {
	auto memory = planMetadataGrant(4096U); QueryPlan empty;
	{
		PlanCache cache(2U, memory);
        const auto baseline = memory->used();
		EXPECT_THROW(cache.put(std::string(10000U, 'x'), empty), ResourceAdmissionDenied);
		EXPECT_EQ(cache.size(), 0U); EXPECT_EQ(memory->used(), baseline);
		cache.put("small", empty);
		EXPECT_GT(memory->used(), 0U); EXPECT_TRUE(cache.get("small"));
	}
	EXPECT_EQ(memory->used(), 0U);
}

TEST(PlanCacheTest, EveryMetadataAllocationFailureLeavesNoUnindexedPlanAndAllowsRetry) {
	auto memory = std::make_shared<FailingPlanMetadata>(planMetadataGrant(65536U));
	QueryPlan empty; const std::string key(1000U, 'x'); size_t allocations = 0U;
	{
		PlanCache probe(2U, memory); memory->calls = 0U; probe.put(key, empty); allocations = memory->calls;
	}
	ASSERT_GE(allocations, 4U); ASSERT_EQ(memory->grant->used(), 0U);
	for (size_t failure = 1U; failure <= allocations; ++failure) {
		SCOPED_TRACE(failure); memory->fail_at = (std::numeric_limits<size_t>::max)();
		{
			PlanCache cache(2U, memory);
            memory->calls = 0U; memory->fail_at = failure;
			EXPECT_THROW(cache.put(key, empty), std::bad_alloc);
			EXPECT_EQ(cache.size(), 0U);
			memory->fail_at = (std::numeric_limits<size_t>::max)();
			cache.put(key, empty); ASSERT_TRUE(cache.get(key)); EXPECT_EQ(cache.size(), 1U);
		}
		EXPECT_EQ(memory->grant->used(), 0U);
	}
}

TEST(PlanCacheTest, CacheHitsAndUpdatesDoNotAllocateAnotherQueryTextOrQueueNode) {
	auto memory = std::make_shared<FailingPlanMetadata>(planMetadataGrant(65536U));
	PlanCache cache(2U, memory); QueryPlan empty; const std::string key(1000U, 'x');
	cache.put(key, empty); const size_t calls = memory->calls;
	memory->fail_at = calls + 1U;
	EXPECT_TRUE(cache.get(key)); EXPECT_NO_THROW(cache.put(key, empty));
	EXPECT_EQ(memory->calls, calls); EXPECT_EQ(cache.size(), 1U);
}

TEST(PlanCacheTest, CacheRetainsItsAllocatorUntilItsLastOwnedMetadataIsDestroyed) {
	auto memory = planMetadataGrant(65536U); std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
	{
		PlanCache cache(2U, memory); memory.reset(); QueryPlan empty;
		cache.put(std::string(1000U, 'x'), empty);
		ASSERT_FALSE(lifetime.expired()); EXPECT_GT(lifetime.lock()->used(), 1000U);
	}
	EXPECT_TRUE(lifetime.expired());
}

TEST(PlanCacheTest, OperatorObjectsRetainTheirResourceAfterTheAllocationScopeEnds) {
    auto memory = planMetadataGrant(4096U);
    std::weak_ptr<std::pmr::memory_resource> lifetime = memory;
    std::unique_ptr<LogicalOperator> root;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        root = std::make_unique<LogicalNodeScan>("n");
    }
    EXPECT_GT(memory->used(), sizeof(LogicalNodeScan));
    memory.reset();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_EQ(root->getOutputVariables(), std::vector<std::string>{"n"});
    root.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST(PlanCacheTest, CachedOperatorAndHitCloneUseSeparateAllocationOwners) {
    auto cacheMemory = planMetadataGrant(16384U);
    auto callerMemory = planMetadataGrant(16384U);
    std::optional<QueryPlan> hit;
    {
        PlanCache cache(2U, cacheMemory);
        QueryPlan source;
        source.root = std::make_unique<LogicalNodeScan>("n");
        cache.put("query", source);
        const auto retained = cacheMemory->used();
        {
            graph::utils::OwnedAllocationScope scope(callerMemory);
            hit = cache.get("query");
        }
        ASSERT_TRUE(hit);
        EXPECT_GT(callerMemory->used(), sizeof(LogicalNodeScan));
        EXPECT_EQ(cacheMemory->used(), retained);
    }
    EXPECT_EQ(cacheMemory->used(), 0U);
    EXPECT_EQ(hit->root->getOutputVariables(), std::vector<std::string>{"n"});
    hit.reset();
    EXPECT_EQ(callerMemory->used(), 0U);
}

TEST(PlanCacheTest, OperatorAdmissionFailureCannotReplaceAnExistingCachedPlan) {
    auto memory = planMetadataGrant(16384U);
    PlanCache cache(2U, memory);
    QueryPlan source; source.root = std::make_unique<LogicalNodeScan>("n");
    cache.put("query", source);
    const auto retained = memory->used();
    PrepaidMemoryResource::ExternalReservation pressure(memory, 16384U - retained);
    EXPECT_THROW(cache.put("query", source), ResourceAdmissionDenied);
    EXPECT_EQ(memory->used(), 16384U);
    EXPECT_EQ(cache.size(), 1U);
    pressure.set(0U);
    EXPECT_EQ(cache.get("query")->root->getOutputVariables(), std::vector<std::string>{"n"});
}

TEST(PlanCacheTest, RebindingIsAtomicAndClearingReleasesEvenEmptyMetadataCapacity) {
    auto first = planMetadataGrant(16384U);
    auto second = planMetadataGrant(16384U);
    auto denied = planMetadataGrant(1U);
    PlanCache cache(2U, first);
    QueryPlan plan; plan.root = std::make_unique<LogicalNodeScan>("n");
    cache.put("query", plan);
    EXPECT_THROW(cache.bindMemoryOwner(denied), ResourceAdmissionDenied);
    EXPECT_EQ(denied->used(), 0U);
    ASSERT_TRUE(cache.get("query"));
    cache.bindMemoryOwner(second);
    EXPECT_EQ(first->used(), 0U);
    EXPECT_EQ(cache.size(), 0U);
    EXPECT_GT(second->used(), 0U);
    cache.put("other", plan);
    cache.clear();
    EXPECT_EQ(second->used(), 0U);
    EXPECT_EQ(cache.size(), 0U);
    EXPECT_FALSE(cache.get("other"));
    cache.bindMemoryOwner(second);
    cache.put("retry", plan);
    ASSERT_TRUE(cache.get("retry"));
}

TEST(PlanCacheTest, ClearPreservesAdmissionAuthorityForTheNextInsertion) {
    auto memory = planMetadataGrant(16384U);
    PlanCache cache(2U, memory);
    QueryPlan plan; plan.root = std::make_unique<LogicalNodeScan>("n");
    cache.put("first", plan);
    cache.clear();
    EXPECT_EQ(memory->used(), 0U);
    {
        PrepaidMemoryResource::ExternalReservation pressure(memory, 16384U);
        EXPECT_THROW(cache.put("second", plan), ResourceAdmissionDenied);
        EXPECT_EQ(cache.size(), 0U);
    }
    cache.put("second", plan);
    EXPECT_GT(memory->used(), sizeof(LogicalNodeScan));
    cache.releaseMemoryOwner();
    EXPECT_EQ(memory->used(), 0U);
}

namespace {
class RejectingOperatorConstruction final : public LogicalNodeScan {
public:
    RejectingOperatorConstruction() : LogicalNodeScan("n") { throw std::runtime_error("constructor failed"); }
};
}
TEST(PlanCacheTest, FailedOperatorConstructionRefundsItsInheritedAllocation) {
    auto memory = planMetadataGrant(4096U);
    graph::utils::OwnedAllocationScope scope(memory);
    EXPECT_THROW(static_cast<void>(std::make_unique<RejectingOperatorConstruction>()), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
}

TEST(PlanCacheTest, IndexBorrowsTheOwnedQueryTextInsteadOfAllocatingASecondCopy) {
    auto memory = std::make_shared<FailingPlanMetadata>(planMetadataGrant(65536U));
    PlanCache cache(2U, memory); QueryPlan plan;
    const std::string query(8192U, 'q');
    memory->large_threshold = query.size();
    cache.put(query, plan);
    EXPECT_EQ(memory->large_requests, 1U);
    EXPECT_TRUE(cache.get(query));
    EXPECT_EQ(memory->large_requests, 1U);
}

TEST(PlanCacheTest, BorrowedIndexKeysSurviveCallerMutationRehashUpdatesAndLruSplices) {
    PlanCache cache(64U);
    QueryPlan first; first.root = std::make_unique<LogicalNodeScan>("first");
    QueryPlan second; second.root = std::make_unique<LogicalNodeScan>("second");
    for (size_t index = 0U; index < 96U; ++index) {
        std::string key = "query-" + std::to_string(index);
        cache.put(key, first);
        key.assign(4096U, 'x');
    }
    EXPECT_EQ(cache.size(), 64U);
    for (size_t index = 0U; index < 32U; ++index)
        EXPECT_FALSE(cache.get("query-" + std::to_string(index)));
    for (size_t index = 32U; index < 96U; ++index) {
        const auto key = "query-" + std::to_string(index);
        ASSERT_TRUE(cache.get(key));
        cache.put(key, second);
        EXPECT_EQ(cache.get(key)->root->getOutputVariables(), std::vector<std::string>{"second"});
    }
    cache.clear();
    EXPECT_EQ(cache.size(), 0U);
    cache.put("query-32", first);
    EXPECT_EQ(cache.get("query-32")->root->getOutputVariables(), std::vector<std::string>{"first"});
}

TEST(PlanCacheTest, ColdCacheStateIsOptionalAndFailedFirstInsertionRefundsItsCandidate) {
    auto memory = planMetadataGrant(1U);
    PlanCache cache(2U, memory);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_FALSE(cache.get("missing"));
    QueryPlan empty;
    EXPECT_THROW(cache.put("query", empty), ResourceAdmissionDenied);
    EXPECT_EQ(cache.size(), 0U);
    EXPECT_EQ(memory->used(), 0U);
}
