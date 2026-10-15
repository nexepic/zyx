#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::execution {
namespace {
using namespace detail;

std::shared_ptr<ResourceAccount> accountWithLimit(size_t bytes) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
	return std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, bytes);
}

TEST(CoordinatedMaterializedBudgetTest, IndependentLocalBudgetsShareOneQueryAllowance) {
	auto account = accountWithLimit(100U);
	MaterializedByteBudget first(100U, account), second(100U, account);
	MaterializedByteReservation a(&first), b(&second);
	a.set(60U); b.set(40U);
	EXPECT_EQ(first.availableWithParent(), 0U);
	EXPECT_EQ(second.availableWithParent(), 0U);
	EXPECT_THROW(b.set(41U), ResourceAdmissionDenied);
	EXPECT_EQ(b.bytes(), 40U);
	EXPECT_EQ(second.used(), 40U);
	EXPECT_EQ(account->used(), 100U);
	a.set(59U); b.set(41U);
	EXPECT_EQ(account->used(), 100U);
	b.reset(); a.reset();
	EXPECT_EQ(account->used(), 0U);
	EXPECT_EQ(first.availableWithParent(), 100U);
}

TEST(CoordinatedMaterializedBudgetTest, LocalRejectionDoesNotChangeParentOrSiblingOwners) {
	auto account = accountWithLimit(100U);
	MaterializedByteBudget budget(30U, account);
	MaterializedByteReservation owner(&budget);
	owner.set(20U);
	EXPECT_THROW(owner.set(31U), MaterializedByteLimitExceeded);
	EXPECT_EQ(owner.bytes(), 20U);
	EXPECT_EQ(budget.used(), 20U);
	EXPECT_EQ(account->used(), 20U);
}

TEST(CoordinatedMaterializedBudgetTest, PmrRequestsAndEmbeddedLogicalParentsAreChargedOnce) {
	auto account = accountWithLimit(128U);
	MaterializedByteBudget budget(128U, account);
	void *frame = budget.allocate(64U, alignof(std::max_align_t));
	MaterializedByteReservation source(&budget);
	source.setAccounting(100U, 64U);
	EXPECT_EQ(account->used(), 100U);
	auto destination = source.splitOffAccounting(100U, 64U);
	EXPECT_EQ(account->used(), 100U);
	EXPECT_THROW(destination.setAccounting(100U, 0U), MaterializedByteLimitExceeded);
	EXPECT_EQ(destination.bytes(), 100U);
	EXPECT_EQ(account->used(), 100U);
	destination.reset();
	EXPECT_EQ(account->used(), 64U);
	budget.deallocate(frame, 64U, alignof(std::max_align_t));
	EXPECT_EQ(account->used(), 0U);
}

class FailingAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U;
private:
	void *do_allocate(size_t, size_t) override { ++calls; throw std::bad_alloc(); }
	void do_deallocate(void *, size_t, size_t) override {}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(CoordinatedMaterializedBudgetTest, UpstreamFailureRefundsBothLevelsAndParentDenialPreventsAllocation) {
	auto account = accountWithLimit(100U);
	FailingAllocator upstream;
	MaterializedByteBudget budget(200U, account, &upstream);
	EXPECT_THROW((void)budget.allocate(101U), ResourceAdmissionDenied);
	EXPECT_EQ(upstream.calls, 0U);
	EXPECT_THROW((void)budget.allocate(80U), std::bad_alloc);
	EXPECT_EQ(upstream.calls, 1U);
	EXPECT_EQ(budget.used(), 0U);
	EXPECT_EQ(account->used(), 0U);
}

TEST(CoordinatedMaterializedBudgetTest, SharedParentOutlivesExternalOwnersUntilBudgetDestruction) {
	std::weak_ptr<ResourceAccount> lifetime;
	{
		auto account = accountWithLimit(100U);
		lifetime = account;
		MaterializedByteBudget budget(100U, account);
		account.reset();
		MaterializedByteReservation owner(&budget);
		owner.set(80U);
		ASSERT_FALSE(lifetime.expired());
		EXPECT_EQ(lifetime.lock()->used(), 80U);
	}
	EXPECT_TRUE(lifetime.expired());
}

TEST(CoordinatedMaterializedBudgetTest, ConcurrentLocalBudgetsCannotOverbookTheirSharedParent) {
	auto account = accountWithLimit(100U);
	std::barrier synchronization(2);
	std::atomic<size_t> admitted = 0U, denied = 0U;
	auto run = [&] {
		MaterializedByteBudget budget(100U, account);
		MaterializedByteReservation owner(&budget);
		synchronization.arrive_and_wait();
		try { owner.set(80U); ++admitted; }
		catch (const ResourceAdmissionDenied &) { ++denied; }
		synchronization.arrive_and_wait();
	};
	std::thread first(run), second(run);
	first.join(); second.join();
	EXPECT_EQ(admitted.load(), 1U);
	EXPECT_EQ(denied.load(), 1U);
	EXPECT_EQ(account->used(), 0U);
}

TEST(CoordinatedMaterializedBudgetTest, DatabaseAndProcessPressureApplyAcrossDistinctQueryAccounts) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(120U));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(100U));
	auto first = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 100U);
	auto second = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 100U);
	MaterializedByteBudget a(100U, first), b(100U, second);
	MaterializedByteReservation x(&a), y(&b);
	x.set(60U);
	EXPECT_THROW(y.set(41U), ResourceAdmissionDenied);
	EXPECT_EQ(b.used(), 0U);
	y.set(40U);
	EXPECT_EQ(database->usage().total, 100U);
	EXPECT_EQ(process->usage().total, 100U);
}

TEST(CoordinatedMaterializedBudgetTest, NullParentAndUpstreamAreRejectedBeforeUse) {
	EXPECT_THROW((MaterializedByteBudget(100U, std::shared_ptr<ResourceAccount>{})), std::invalid_argument);
	EXPECT_THROW((MaterializedByteBudget(100U, accountWithLimit(100U), nullptr)), std::invalid_argument);
}

TEST(CoordinatedMaterializedBudgetTest, DetachedOutputRetainsGlobalChargeAfterLocalBudgetDestruction) {
	auto account = accountWithLimit(100U);
	std::optional<ResourceLease> output;
	{
		MaterializedByteBudget budget(100U, account);
		MaterializedByteReservation temporary(&budget), retained(&budget);
		temporary.set(20U); retained.set(80U);
		output.emplace(retained.detachToParent());
		EXPECT_EQ(retained.bytes(), 0U);
		EXPECT_EQ(budget.used(), 20U);
		EXPECT_EQ(account->used(), 100U);
	}
	EXPECT_EQ(account->used(), 80U);
	MaterializedByteBudget next(100U, account);
	MaterializedByteReservation temporary(&next);
	EXPECT_THROW(temporary.set(21U), ResourceAdmissionDenied);
	output.reset(); temporary.set(100U);
	EXPECT_EQ(account->used(), 100U);
}

TEST(CoordinatedMaterializedBudgetTest, InvalidDetachPreservesLogicalAndParentAccounting) {
	MaterializedByteBudget local(100U);
	MaterializedByteReservation localOwner(&local);
	localOwner.set(40U);
	EXPECT_THROW((void)localOwner.detachToParent(), std::logic_error);
	EXPECT_EQ(localOwner.bytes(), 40U);
	EXPECT_EQ(local.used(), 40U);
	auto account = accountWithLimit(100U);
	MaterializedByteBudget budget(100U, account);
	MaterializedByteReservation owner(&budget);
	owner.setAccounting(60U, 20U);
	EXPECT_THROW((void)owner.detachToParent(), std::logic_error);
	EXPECT_EQ(owner.bytes(), 60U);
	EXPECT_EQ(budget.used(), 40U);
	EXPECT_EQ(account->used(), 40U);
}

TEST(CoordinatedMaterializedBudgetTest, DeepCopyAdmissionKeepsOriginalAndAllCountersOnDenial) {
	auto account = accountWithLimit(100U);
	ResourceLease original(account);
	original.set(60U);
	EXPECT_THROW((void)original.duplicate(), ResourceAdmissionDenied);
	EXPECT_EQ(original.bytes(), 60U);
	EXPECT_EQ(account->used(), 60U);
	original.set(40U);
	{
		auto copy = original.duplicate();
		EXPECT_EQ(copy.bytes(), 40U);
		EXPECT_EQ(account->used(), 80U);
		original.reset();
		EXPECT_EQ(account->used(), 40U);
	}
	EXPECT_EQ(account->used(), 0U);
	ResourceLease moved(std::move(original));
	EXPECT_THROW((void)original.duplicate(), std::logic_error);
}

TEST(CoordinatedMaterializedBudgetTest, AdvisoryHeadroomIncludesSiblingCategoriesAndMaintenanceFloor) {
	auto limits = ResourceLimits::uniform(100U);
	limits.protected_maintenance_floor = 20U;
	auto process = std::make_shared<ProcessResourceBudget>(limits);
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(100U));
	auto query = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 100U);
	ResourceAccount cache(database, ResourceClass::RCK_CACHE, 100U);
	ResourceAccount maintenance(database, ResourceClass::RCK_MAINTENANCE, 100U);
	MaterializedByteBudget budget(90U, query);
	ResourceLease cached(cache), maintained(maintenance);
	EXPECT_EQ(budget.available(), 90U);
	EXPECT_EQ(budget.availableWithParent(), 80U);
	cached.set(30U);
	EXPECT_EQ(budget.availableWithParent(), 50U);
	maintained.set(30U);
	EXPECT_EQ(budget.availableWithParent(), 40U);
	MaterializedByteReservation owner(&budget); owner.set(40U);
	EXPECT_EQ(budget.availableWithParent(), 0U);
	maintained.reset();
	EXPECT_EQ(budget.availableWithParent(), 10U);
}
} // namespace
} // namespace graph::query::execution
