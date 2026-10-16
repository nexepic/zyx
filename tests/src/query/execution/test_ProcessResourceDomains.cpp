#include <gtest/gtest.h>
#include <barrier>
#include <thread>
#include <optional>
#include "query/execution/ProcessResourceDomains.hpp"

namespace graph::query::execution::detail {
namespace {
TEST(ProcessResourceDomainsTest, NativeWriteGateSharesFileIdentityAndSeparatesDifferentDomains) {
 ProcessResourceDomains registry(ResourceLimits::uniform(200U),ResourceLimits::uniform(100U));
 auto first=registry.domain("first"),alias=registry.domain("first"),other=registry.domain("other");
 auto firstGate=registry.lockWriteWorkspace(first);
 const auto *identity=firstGate.mutex();
 auto otherGate=registry.lockWriteWorkspace(other);
 EXPECT_NE(identity,otherGate.mutex());
 firstGate.unlock();
 auto aliasGate=registry.lockWriteWorkspace(alias);
 EXPECT_EQ(identity,aliasGate.mutex());
 ProcessResourceDomains foreign(ResourceLimits::uniform(200U),ResourceLimits::uniform(100U));
 auto outsider=foreign.domain("first");
 EXPECT_THROW(static_cast<void>(registry.lockWriteWorkspace(outsider)),std::invalid_argument);
 EXPECT_EQ(first->usage().total,0U);
}

TEST(ProcessResourceDomainsTest, RepeatedIdentityReusesTheDomainAndItsAllowance) {
	ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
	auto first = registry.domain("opened-file:database-id"), alias = registry.domain("opened-file:database-id");
	EXPECT_EQ(first, alias);
	ResourceAccount query(first, ResourceClass::RCK_QUERY, 100U);
	ResourceLease retained(query); retained.set(80U);
	EXPECT_EQ(registry.usage().total, 80U);
	EXPECT_EQ(alias->usage().total, 80U);
}

TEST(ProcessResourceDomainsTest, DifferentDatabasesCannotIndividuallyBypassTheProcessLimit) {
	ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
	auto first = registry.domain("first"), second = registry.domain("second");
	ResourceAccount a(first, ResourceClass::RCK_QUERY, 100U), b(second, ResourceClass::RCK_QUERY, 100U);
	ResourceLease x(a), y(b); x.set(80U);
	EXPECT_THROW(y.set(41U), ResourceAdmissionDenied);
	EXPECT_EQ(second->usage().total, 0U);
	y.set(40U); EXPECT_EQ(registry.usage().total, 120U);
	EXPECT_EQ(b.available(), 0U);
}

TEST(ProcessResourceDomainsTest, RetainedOutputKeepsClosedDomainAndQuerySlotsOccupied) {
	ProcessResourceDomains registry(ResourceLimits::uniform(100U), ResourceLimits::uniform(100U), 1U, 1U);
	std::optional<ResourceLease> output;
	{
		auto domain = registry.domain("first");
		auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 100U);
		output.emplace(account); output->set(50U);
	}
	EXPECT_THROW((void)registry.domain("second"), ResourceAdmissionDenied);
	{
		auto same = registry.domain("first");
		EXPECT_THROW((void)ResourceAccount(same, ResourceClass::RCK_QUERY, 100U), ResourceAdmissionDenied);
	}
	output.reset();
	EXPECT_EQ(registry.usage().total, 0U);
	EXPECT_NO_THROW((void)registry.domain("second"));
}

TEST(ProcessResourceDomainsTest, ConcurrentAliasAdmissionUsesOneFiniteDomainSlot) {
	ProcessResourceDomains registry(ResourceLimits::uniform(100U), ResourceLimits::uniform(100U), 1U);
	std::barrier synchronization(2);
	std::array<std::shared_ptr<DatabaseResourceBudget>, 2U> domains;
	std::array<std::exception_ptr, 2U> errors;
	auto run = [&](size_t index) {
		synchronization.arrive_and_wait();
		try { domains[index] = registry.domain("same"); }
		catch (...) { errors[index] = std::current_exception(); }
	};
	std::thread first(run, 0U), second(run, 1U); first.join(); second.join();
	EXPECT_FALSE(errors[0]); EXPECT_FALSE(errors[1]);
	ASSERT_TRUE(domains[0]); EXPECT_EQ(domains[0], domains[1]);
}

TEST(ProcessResourceDomainsTest, InvalidConfigurationAndUnboundedIdentitiesAreRejected) {
	EXPECT_THROW((void)ProcessResourceDomains(ResourceLimits::uniform(100U), ResourceLimits::uniform(101U)), std::invalid_argument);
	EXPECT_THROW((void)ProcessResourceDomains(ResourceLimits::uniform(100U), ResourceLimits::uniform(100U), 65U), std::invalid_argument);
	ProcessResourceDomains registry(ResourceLimits::uniform(100U), ResourceLimits::uniform(100U));
	EXPECT_THROW((void)registry.domain(""), std::invalid_argument);
	EXPECT_THROW((void)registry.domain(std::string(257U, 'x')), std::invalid_argument);
	EXPECT_EQ(registry.usage().total, 0U);
}
} // namespace
} // namespace graph::query::execution::detail

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, CacheGrantIsSharedByAliasesAndRefundedAfterTheLastOwner) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto domain = registry.domain("same");
    auto first = registry.cacheGrant(domain);
    auto alias = registry.cacheGrant(registry.domain("same"));
    EXPECT_EQ(first, alias);
    EXPECT_EQ(registry.usage().total, 100U);
    first.reset();
    EXPECT_EQ(registry.usage().total, 100U);
    alias.reset();
    EXPECT_EQ(registry.usage().total, 0U);
    EXPECT_EQ(registry.cacheGrant(domain)->capacity(), 100U);
}

TEST(ProcessResourceDomainsTest, FailedCacheGrantAdmissionDoesNotPublishOrLeakAnAllowance) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto firstDomain = registry.domain("first"), secondDomain = registry.domain("second");
    auto first = registry.cacheGrant(firstDomain);
    EXPECT_THROW(static_cast<void>(registry.cacheGrant(secondDomain)), ResourceAdmissionDenied);
    EXPECT_EQ(secondDomain->usage().total, 0U);
    EXPECT_EQ(registry.usage().total, 100U);
    first.reset();
    EXPECT_NO_THROW(static_cast<void>(registry.cacheGrant(secondDomain)));
    auto foreign = std::make_shared<DatabaseResourceBudget>(
        std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U)), ResourceLimits::uniform(100U));
    EXPECT_THROW(static_cast<void>(registry.cacheGrant(foreign)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(registry.cacheGrant({})), std::invalid_argument);
}
}

namespace graph::query::execution::detail {
namespace {
class DomainDefaultProbe final : public std::pmr::memory_resource {
public:
    size_t allocations = 0U;
private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        ++allocations; return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return &other == this; }
};
}
TEST(ProcessResourceDomainsTest, NativeCacheGrantUsesAnUpstreamWithProcessLifetime) {
    DomainDefaultProbe temporary;
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto domain = registry.domain("native");
    std::shared_ptr<PrepaidMemoryResource> memory;
    {
        auto *previous = std::pmr::set_default_resource(&temporary);
        struct Restore { std::pmr::memory_resource *previous; ~Restore() { std::pmr::set_default_resource(previous); } } restore{previous};
        memory = registry.cacheGrant(domain);
    }
    void *buffer = memory->allocate(32U);
    EXPECT_EQ(temporary.allocations, 0U);
    memory->deallocate(buffer, 32U);
    EXPECT_EQ(memory->used(), 0U);
}
}

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, ProtectedControlGrantIsSharedAndUsesOnlyTheMaintenanceClass) {
    auto process = ResourceLimits::uniform(120U);
    auto database = ResourceLimits::uniform(100U);
    database.classes[static_cast<size_t>(ResourceClass::RCK_CACHE)] = 60U;
    database.classes[static_cast<size_t>(ResourceClass::RCK_MAINTENANCE)] = 40U;
    database.protected_maintenance_floor = 40U;
    ProcessResourceDomains registry(process, database);
    auto domain = registry.domain("controls");
    auto cache = registry.cacheGrant(domain);
    auto controls = registry.controlGrant(domain);
    EXPECT_EQ(controls->capacity(), 40U);
    EXPECT_EQ(registry.controlGrant(registry.domain("controls")), controls);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_CACHE)], 60U);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_MAINTENANCE)], 40U);
    EXPECT_EQ(domain->usage().total, 100U);
    controls.reset();
    EXPECT_EQ(domain->usage().total, 60U);
    cache.reset();
    EXPECT_EQ(registry.usage().total, 0U);
}

TEST(ProcessResourceDomainsTest, FailedControlGrantDoesNotPublishOrRetainForeignDomains) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto firstDomain = registry.domain("first"), secondDomain = registry.domain("second");
    auto first = registry.controlGrant(firstDomain);
    EXPECT_THROW((void)registry.controlGrant(secondDomain), ResourceAdmissionDenied);
    EXPECT_EQ(secondDomain->usage().total, 0U);
    first.reset();
    EXPECT_NO_THROW((void)registry.controlGrant(secondDomain));
    EXPECT_THROW((void)registry.controlGrant({}), std::invalid_argument);
    auto foreign = std::make_shared<DatabaseResourceBudget>(
        std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U)), ResourceLimits::uniform(100U));
    EXPECT_THROW((void)registry.controlGrant(foreign), std::invalid_argument);
}
}

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, LogicalIdRegistryGrantUsesWriteClassAndIsSharedByAliases) {
    auto limits = ResourceLimits::uniform(100U);
    limits.classes[static_cast<size_t>(ResourceClass::RCK_CACHE)] = 60U;
    limits.classes[static_cast<size_t>(ResourceClass::RCK_WRITE)] = 40U;
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), limits);
    auto domain = registry.domain("capabilities");
    auto cache = registry.cacheGrant(domain);
    auto first = registry.logicalIdGrant(domain);
    auto alias = registry.logicalIdGrant(registry.domain("capabilities"));
    EXPECT_EQ(first, alias);
    EXPECT_EQ(first->capacity(), 40U);
    EXPECT_EQ(domain->usage().classes[static_cast<size_t>(ResourceClass::RCK_WRITE)], 40U);
    EXPECT_EQ(domain->usage().total, 100U);
    first.reset(); EXPECT_EQ(domain->usage().total, 100U);
    alias.reset(); EXPECT_EQ(domain->usage().total, 60U);
    cache.reset(); EXPECT_EQ(registry.usage().total, 0U);
}

TEST(ProcessResourceDomainsTest, DeniedLogicalIdGrantCannotPublishOrLeakCapacity) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto firstDomain = registry.domain("first"), secondDomain = registry.domain("second");
    auto first = registry.logicalIdGrant(firstDomain);
    EXPECT_THROW((void)registry.logicalIdGrant(secondDomain), ResourceAdmissionDenied);
    EXPECT_EQ(secondDomain->usage().total, 0U);
    first.reset();
    EXPECT_NO_THROW((void)registry.logicalIdGrant(secondDomain));
    EXPECT_THROW((void)registry.logicalIdGrant({}), std::invalid_argument);
    auto foreign = std::make_shared<DatabaseResourceBudget>(
        std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U)), ResourceLimits::uniform(100U));
    EXPECT_THROW((void)registry.logicalIdGrant(foreign), std::invalid_argument);
}
}

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, DefaultDatabaseRetainsMaintenanceHeadroomUnderSaturatedForegroundAdmission) {
    ProcessResourceDomains registry;
    auto domain = registry.domain("saturated-native-database");
    auto cache = registry.cacheGrant(domain);
    auto controls = registry.controlGrant(domain);
    auto capabilities = registry.logicalIdGrant(domain);
    ResourceAccount foreground(domain, ResourceClass::RCK_QUERY, 192U * 1024U * 1024U);
    ResourceLease pressure(foreground);
    pressure.set(foreground.available());
    EXPECT_EQ(foreground.available(), 0U);
    EXPECT_THROW(pressure.set(pressure.bytes() + 1U), ResourceAdmissionDenied);
    EXPECT_EQ(domain->usage().total, 241U * 1024U * 1024U);
    ResourceAccount maintenance(domain, ResourceClass::RCK_MAINTENANCE, 32U * 1024U * 1024U);
    ResourceLease workspace(maintenance);
    EXPECT_NO_THROW(workspace.set(8U * 1024U * 1024U));
    EXPECT_EQ(domain->usage().total, 249U * 1024U * 1024U);
    workspace.reset();
    EXPECT_EQ(foreground.available(), 0U);
    pressure.reset();
    EXPECT_GT(foreground.available(), 0U);
}
}

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, WritableAliasesCannotRaiseOrLowerAnActivePhysicalFileCeiling) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U));
    auto domain = registry.domain("shared-file");
    auto first = registry.fileLimit(domain, 8192U);
    auto alias = registry.fileLimit(registry.domain("shared-file"), 8192U);
    EXPECT_EQ(first, alias); EXPECT_EQ(*first, 8192U);
    EXPECT_THROW((void)registry.fileLimit(domain, 8193U), std::invalid_argument);
    EXPECT_THROW((void)registry.fileLimit(domain, 8191U), std::invalid_argument);
    EXPECT_THROW((void)registry.fileLimit(domain, 0U), std::invalid_argument);
    EXPECT_THROW((void)registry.fileLimit({}, 8192U), std::invalid_argument);
    first.reset(); alias.reset();
    EXPECT_EQ(*registry.fileLimit(domain, 16384U), 16384U);
    EXPECT_EQ(registry.usage().total, 0U);
}
}

namespace graph::query::execution::detail {
TEST(ProcessResourceDomainsTest, RetainedFileLimitValueCannotBindAReusedSlotToAnotherDatabase) {
    ProcessResourceDomains registry(ResourceLimits::uniform(120U), ResourceLimits::uniform(100U), 1U);
    auto first = registry.domain("first");
    auto oldLimit = registry.fileLimit(first, 8192U); first.reset();
    auto second = registry.domain("second");
    EXPECT_EQ(*registry.fileLimit(second, 16384U), 16384U);
    EXPECT_EQ(*oldLimit, 8192U);
}
}
