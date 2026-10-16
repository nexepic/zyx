#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "query/execution/ResourceCoordinator.hpp"
#include "query/execution/ProcedurePayloadAdmission.hpp"

namespace graph::query::execution::detail {
namespace {
TEST(ResourceCoordinatorTest, ProcessDatabaseAndSharedAccountLimitsAreAtomicAndRefundable) {
	ProcessResourceBudget process(ResourceLimits::uniform(100U));
	DatabaseResourceBudget database(process, ResourceLimits::uniform(60U));
	ResourceAccount first(database, ResourceClass::RCK_QUERY, 40U), second(database, ResourceClass::RCK_QUERY, 40U);
	ResourceLease a(first), b(first), c(second);
	a.set(30U);
	EXPECT_FALSE(b.trySet(11U));
	EXPECT_EQ(first.used(), 30U);
	b.set(10U);
	c.set(20U);
	EXPECT_FALSE(c.trySet(21U));
	EXPECT_EQ(database.usage().total, 60U);
	EXPECT_EQ(process.usage().total, 60U);
	a.reset();
	c.set(40U);
	EXPECT_EQ(process.usage().total, 50U);
	b.reset(); c.reset();
	EXPECT_EQ(process.usage().total, 0U);
	EXPECT_EQ(first.used(), 0U);
	EXPECT_EQ(second.used(), 0U);
}

TEST(ResourceCoordinatorTest, CategoriesAreSingleAttributedAndRespectGlobalCapsAcrossDatabases) {
	auto limits = ResourceLimits::uniform(100U);
	limits.classes[static_cast<size_t>(ResourceClass::RCK_CACHE)] = 10U;
	limits.classes[static_cast<size_t>(ResourceClass::RCK_CODE)] = 0U;
	ProcessResourceBudget process(limits);
	DatabaseResourceBudget first(process, ResourceLimits::uniform(100U)), second(process, ResourceLimits::uniform(100U));
	ResourceAccount cacheA(first, ResourceClass::RCK_CACHE, 100U), cacheB(second, ResourceClass::RCK_CACHE, 100U);
	ResourceAccount query(second, ResourceClass::RCK_QUERY, 100U), code(first, ResourceClass::RCK_CODE, 100U);
	ResourceLease a(cacheA), b(cacheB), q(query), c(code);
	a.set(6U); b.set(4U); q.set(90U);
	EXPECT_FALSE(b.trySet(5U));
	EXPECT_FALSE(c.trySet(1U));
	const auto usage = process.usage();
	EXPECT_EQ(usage.total, 100U);
	EXPECT_EQ(usage.classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 10U);
	EXPECT_EQ(usage.classes[static_cast<size_t>(ResourceClass::RCK_QUERY)], 90U);
	EXPECT_EQ(second.usage().total, 94U);
}

TEST(ResourceCoordinatorTest, DatabaseCategoryCapsAndOwnerTransfersCannotMultiplyQuota) {
	ProcessResourceBudget process(ResourceLimits::uniform(100U));
	auto limits = ResourceLimits::uniform(100U);
	limits.classes[static_cast<size_t>(ResourceClass::RCK_WRITE)] = 30U;
	DatabaseResourceBudget database(process, limits);
	ResourceAccount account(database, ResourceClass::RCK_WRITE, 50U), other(database, ResourceClass::RCK_QUERY, 50U);
	ResourceLease owner(account), foreign(other);
	owner.set(30U);
	auto part = owner.splitOff(20U);
	EXPECT_EQ(process.usage().total, 30U);
	EXPECT_FALSE(owner.trySet(11U));
	EXPECT_THROW(owner.absorb(std::move(foreign)), std::logic_error);
	EXPECT_THROW(static_cast<void>(owner.splitOff(11U)), std::logic_error);
	owner.absorb(std::move(part));
	EXPECT_EQ(owner.bytes(), 30U);
	EXPECT_EQ(part.bytes(), 0U);
	EXPECT_EQ(account.used(), 30U);
	owner.reset();
	EXPECT_EQ(process.usage().total, 0U);
}

TEST(ResourceCoordinatorTest, MoveAssignmentRefundsPreviousAccountAndTransfersExactlyOnce) {
	ProcessResourceBudget process(ResourceLimits::uniform(100U));
	DatabaseResourceBudget database(process, ResourceLimits::uniform(100U));
	ResourceAccount first(database, ResourceClass::RCK_QUERY, 100U), second(database, ResourceClass::RCK_CACHE, 100U);
	{
		ResourceLease a(first), b(second);
		a.set(20U); b.set(30U);
		b = std::move(a);
		EXPECT_EQ(process.usage().total, 20U);
		EXPECT_EQ(second.used(), 0U);
		EXPECT_EQ(a.bytes(), 0U);
		EXPECT_THROW(a.set(1U), std::logic_error);
		EXPECT_EQ(b.bytes(), 20U);
		EXPECT_EQ(process.usage().total, 20U);
	}
	EXPECT_EQ(first.used(), 0U);
	EXPECT_EQ(process.usage().total, 0U);
}

class CoordinatedUpstream final : public std::pmr::memory_resource {
public:
	size_t calls = 0U, live = 0U;
	bool fail = false;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		++calls;
		if (fail) throw std::bad_alloc();
		void *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live += bytes;
		return result;
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
		live -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(ResourceCoordinatorTest, AllocatorAdmitsBeforeUpstreamRefundsBadAllocAndChargesGrowthOverlap) {
	ProcessResourceBudget process(ResourceLimits::uniform(128U));
	DatabaseResourceBudget database(process, ResourceLimits::uniform(128U));
	ResourceAccount account(database, ResourceClass::RCK_QUERY, 128U);
	CoordinatedUpstream upstream;
	CoordinatedMemoryResource memory(account, &upstream);
	EXPECT_THROW(static_cast<void>(memory.allocate(129U)), ResourceAdmissionDenied);
	EXPECT_EQ(upstream.calls, 0U);
	upstream.fail = true;
	EXPECT_THROW(static_cast<void>(memory.allocate(8U)), std::bad_alloc);
	EXPECT_EQ(process.usage().total, 0U);
	upstream.fail = false;
	{
		std::pmr::vector<uint64_t> values(&memory);
		values.reserve(8U);
		EXPECT_EQ(account.used(), values.capacity() * sizeof(uint64_t));
		EXPECT_THROW(values.reserve(16U), ResourceAdmissionDenied); // Old/new overlap is 192 bytes.
		EXPECT_EQ(values.capacity(), 8U);
		EXPECT_EQ(account.used(), 64U);
		EXPECT_EQ(upstream.live, 64U);
	}
	EXPECT_EQ(process.usage().total, 0U);
	EXPECT_EQ(upstream.live, 0U);
	EXPECT_THROW((CoordinatedMemoryResource(account, nullptr)), std::invalid_argument);
}

TEST(ResourceCoordinatorTest, ConcurrentAccountsAcrossDatabasesCannotOverbookProcessTotal) {
	ProcessResourceBudget process(ResourceLimits::uniform(16U));
	DatabaseResourceBudget first(process, ResourceLimits::uniform(16U)), second(process, ResourceLimits::uniform(16U));
	ResourceAccount a(first, ResourceClass::RCK_QUERY, 16U), b(second, ResourceClass::RCK_QUERY, 16U);
	std::barrier boundary(2);
	std::atomic<size_t> admitted{0U};
	const auto run = [&](ResourceAccount &account) {
		ResourceLease lease(account);
		boundary.arrive_and_wait();
		if (lease.trySet(16U)) ++admitted;
		boundary.arrive_and_wait(); // Both attempts finish while the successful lease is still held.
		EXPECT_EQ(process.usage().total, 16U);
		boundary.arrive_and_wait();
	};
	std::thread one(run, std::ref(a)), two(run, std::ref(b));
	one.join(); two.join();
	EXPECT_EQ(admitted.load(), 1U);
	EXPECT_EQ(process.usage().total, 0U);
	EXPECT_EQ(a.used(), 0U);
	EXPECT_EQ(b.used(), 0U);
}

TEST(ResourceCoordinatorTest, CheckedLimitsAndMaximumSizeAdmissionNeverWrap) {
	EXPECT_THROW((ProcessResourceBudget(ResourceLimits::uniform(0U))), std::invalid_argument);
	auto invalid = ResourceLimits::uniform(10U);
	invalid.classes[0] = 11U;
	EXPECT_THROW((ProcessResourceBudget(invalid)), std::invalid_argument);
	ProcessResourceBudget process(ResourceLimits::uniform((std::numeric_limits<size_t>::max)()));
	DatabaseResourceBudget database(process, ResourceLimits::uniform((std::numeric_limits<size_t>::max)()));
	EXPECT_THROW((ResourceAccount(database, ResourceClass::RCK_COUNT, 1U)), std::invalid_argument);
	EXPECT_THROW((ResourceAccount(database, ResourceClass::RCK_QUERY, 0U)), std::invalid_argument);
	ResourceAccount account(database, ResourceClass::RCK_QUERY, (std::numeric_limits<size_t>::max)());
	ResourceLease a(account), b(account);
	a.set((std::numeric_limits<size_t>::max)());
	EXPECT_FALSE(b.trySet(1U));
	EXPECT_EQ(process.usage().total, (std::numeric_limits<size_t>::max)());
	a.reset();
	b.set(1U);
	EXPECT_EQ(process.usage().total, 1U);
}
TEST(ResourceCoordinatorTest, OwnedLeaseKeepsHierarchyAliveAfterDatabaseOwnerIsReleased) {
	auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U));
	auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(80U));
	auto account=std::make_shared<ResourceAccount>(database,ResourceClass::RCK_QUERY,60U);
	std::weak_ptr<ProcessResourceBudget> processLife=process;
	std::weak_ptr<DatabaseResourceBudget> databaseLife=database;
	std::weak_ptr<ResourceAccount> accountLife=account;
	{
		ResourceLease retained(account);retained.set(50U);
		auto transferred=retained.splitOff(20U);
		ResourceLease borrowed(*account);borrowed.absorb(std::move(transferred));
		transferred=std::move(borrowed);
		account.reset();database.reset();process.reset();
		EXPECT_FALSE(processLife.expired());EXPECT_FALSE(databaseLife.expired());EXPECT_FALSE(accountLife.expired());
		EXPECT_EQ(processLife.lock()->usage().total,50U);
		retained.reset();EXPECT_EQ(processLife.lock()->usage().total,20U);
		ResourceLease moved(std::move(transferred));moved.set(30U);
		EXPECT_EQ(processLife.lock()->usage().total,30U);
	}
	EXPECT_TRUE(accountLife.expired());EXPECT_TRUE(databaseLife.expired());EXPECT_TRUE(processLife.expired());
}
TEST(ResourceCoordinatorTest, OwnedHierarchyRejectsNullOwners) {
	EXPECT_THROW(DatabaseResourceBudget(std::shared_ptr<ProcessResourceBudget>{},ResourceLimits::uniform(10U)),std::invalid_argument);
	EXPECT_THROW(ResourceAccount(std::shared_ptr<DatabaseResourceBudget>{},ResourceClass::RCK_QUERY,10U),std::invalid_argument);
	EXPECT_THROW(ResourceLease(std::shared_ptr<ResourceAccount>{}),std::invalid_argument);
}
TEST(ResourceCoordinatorTest, QueryPressureCannotConsumeProtectedMaintenanceFloor) {
	auto limits=ResourceLimits::uniform(100U);limits.protected_maintenance_floor=20U;
	ProcessResourceBudget process(limits);DatabaseResourceBudget database(process,ResourceLimits::uniform(100U));
	ResourceAccount query(database,ResourceClass::RCK_QUERY,100U),maintenance(database,ResourceClass::RCK_MAINTENANCE,100U);
	ResourceLease foreground(query),background(maintenance);foreground.set(80U);
	EXPECT_FALSE(foreground.trySet(81U));EXPECT_EQ(process.usage().total,80U);
	background.set(20U);EXPECT_EQ(process.usage().total,100U);
	background.reset();EXPECT_FALSE(foreground.trySet(81U));
	foreground.set(70U);background.set(30U);EXPECT_EQ(process.usage().total,100U);
	limits.protected_maintenance_floor=101U;EXPECT_THROW(ProcessResourceBudget invalid(limits),std::invalid_argument);
}
TEST(ResourceCoordinatorTest, DatabaseAndQuerySlotsRemainHeldBySharedAccountLeases) {
	auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U),1U);
	auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(100U),1U);
	auto account=std::make_shared<ResourceAccount>(database,ResourceClass::RCK_QUERY,100U);
	{
		ResourceLease retained(account);retained.set(10U);account.reset();
		EXPECT_THROW(ResourceAccount denied(*database,ResourceClass::RCK_QUERY,100U),ResourceAdmissionDenied);
		database.reset();EXPECT_THROW(DatabaseResourceBudget denied(process,ResourceLimits::uniform(100U)),ResourceAdmissionDenied);
		EXPECT_EQ(process->usage().total,10U);
	}
	EXPECT_EQ(process->usage().total,0U);
	EXPECT_NO_THROW(DatabaseResourceBudget replacement(process,ResourceLimits::uniform(100U)));
}
} // namespace
} // namespace graph::query::execution::detail

namespace graph::query::execution::detail {
TEST(ProcedurePayloadHandoffTest, ReturnedReservationStaysChargedButFreedPmrBackingRefunds) {
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(4096U));
    auto database = std::make_shared<DatabaseResourceBudget>(*process, ResourceLimits::uniform(4096U));
    auto account = std::make_shared<ResourceAccount>(*database, ResourceClass::RCK_QUERY, 4096U);
    auto target = std::make_shared<MaterializedByteBudget>(4096U, account);
    utils::OwnedAllocationScope owner(target);
    {
        ProcedurePayloadCredit credit;
        ProcedurePayloadAdmissionScope scope(credit);
        {
            MaterializedByteBudget source(4096U, account);
            ProcedureBudgetHandoff handoff(source);
            MaterializedByteReservation returned(&source);
            returned.set(2048U);
            void *scratch = source.allocate(1024U);
            EXPECT_EQ(account->used(), 3072U);
            source.deallocate(scratch, 1024U);
            EXPECT_EQ(account->used(), 2048U);
            EXPECT_EQ(credit.bytes(), 0U);
            returned.reset();
            EXPECT_EQ(account->used(), 2048U);
            EXPECT_EQ(source.used(), 0U);
            EXPECT_EQ(target->used(), 2048U);
            EXPECT_EQ(credit.bytes(), 2048U);
            EXPECT_THROW(target->reserveUnmanaged(2049U), MaterializedByteLimitExceeded);
            EXPECT_EQ(account->used(), 2048U);
        }
        EXPECT_EQ(account->used(), 2048U);
    }
    EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(target->used(), 0U);
}

TEST(ProcedurePayloadHandoffTest, ForeignAndUnparentedBudgetsKeepNormalRefundBehavior) {
    ProcessResourceBudget process(ResourceLimits::uniform(8192U));
    DatabaseResourceBudget database(process, ResourceLimits::uniform(8192U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 4096U);
    auto foreign = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 4096U);
    auto target = std::make_shared<MaterializedByteBudget>(4096U, account);
    utils::OwnedAllocationScope owner(target);
    ProcedurePayloadCredit credit;
    ProcedurePayloadAdmissionScope scope(credit);
    for (bool parented : {false, true}) {
        auto source = parented ? std::make_unique<MaterializedByteBudget>(4096U, foreign)
                               : std::make_unique<MaterializedByteBudget>(4096U);
        ProcedureBudgetHandoff handoff(*source);
        EXPECT_FALSE(source->transfersReservations());
        MaterializedByteReservation temporary(source.get()); temporary.set(1024U);
        temporary.reset(); EXPECT_EQ(source->used(), 0U);
        EXPECT_EQ(credit.bytes(), 0U); EXPECT_EQ(target->used(), 0U);
    }
}
} // namespace graph::query::execution::detail

namespace graph::query::execution::detail {
TEST(ProcedurePayloadHandoffTest, PublishedRowTransfersItsChargeAndOutlivesTheProcedureScope) {
    ProcessResourceBudget process(ResourceLimits::uniform(32768U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(32768U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 32768U);
    auto memory = std::make_shared<MaterializedByteBudget>(32768U, account);
    QueryResult result{ResourceLease(account)};
    result.reserveRows(1U);
    const size_t emptyResult = result.retainedResourceBytes();
    {
        utils::OwnedAllocationScope owner(memory);
        ProcedurePayloadCredit credit;
        ProcedurePayloadAdmissionScope payload(credit);
        credit.add(16384U); // Granted before constructing the public raw row.
        QueryResult::Row row;
        row.emplace("payload", PropertyValue(std::string(2000U, 'x')));
        const auto rowFee = graph::query::detail::resultRowHeapBytes(row);
        ProcedureResultHandoff::append(result, std::move(row), credit);
        EXPECT_EQ(result.retainedResourceBytes(), emptyResult + rowFee);
        EXPECT_EQ(credit.bytes(), 16384U - rowFee);
        EXPECT_EQ(account->used(), emptyResult + 16384U);
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(account->used(), result.retainedResourceBytes());
    EXPECT_EQ(result.getRows().front().at("payload").asPrimitive(), PropertyValue(std::string(2000U, 'x')));
    result = QueryResult{}; EXPECT_EQ(account->used(), 0U);
}

TEST(ProcedurePayloadHandoffTest, DistinctKeyRetainsIndependentCreditAcrossRowRelease) {
    auto memory = std::make_shared<MaterializedByteBudget>(32768U);
    std::weak_ptr<std::pmr::memory_resource> weak = memory;
    std::optional<utils::OwnedStdString> escaped;
    {
        utils::OwnedAllocationScope owner(memory);
        {
            ProcedurePayloadCredit credit;
            ProcedurePayloadAdmissionScope payload(credit);
            credit.add(8192U);
            LogicalPlanReadRow row;
            row.values.emplace("payload", PropertyValue(std::string(1000U, 'v')));
            escaped.emplace(makeOwnedProcedureDistinctRowKey(row, {"payload"}));
            EXPECT_EQ(*escaped, LogicalPlanReadRuntime::makeDistinctRowKey(row, {"payload"}));
            EXPECT_GT(memory->used(), credit.bytes());
        }
        EXPECT_GT(memory->used(), 1000U);
        auto denied = std::make_shared<MaterializedByteBudget>(1U);
        utils::OwnedAllocationScope low(denied);
        EXPECT_THROW(static_cast<void>(utils::OwnedStdString(*escaped)), MaterializedByteLimitExceeded);
        EXPECT_EQ(denied->used(), 0U);
    }
    memory.reset(); EXPECT_FALSE(weak.expired());
    escaped.reset(); EXPECT_TRUE(weak.expired());
}
} // namespace graph::query::execution::detail
