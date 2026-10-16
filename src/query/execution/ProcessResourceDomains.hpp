#pragma once

#include <array>
#include <string>
#include "graph/query/execution/ResourceCoordinator.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"

namespace graph::query::execution::detail {
// Registry identity comes from an opened native file plus its database ID.
// Weak entries do not keep closed domains alive; retained allocation leases do.
class ProcessResourceDomains final {
public:
	static ResourceLimits processDefaults() {
		auto limits = ResourceLimits::uniform(512U * 1024U * 1024U);
		limits.classes[static_cast<size_t>(ResourceClass::RCK_CODE)] = 0U;
		limits.protected_maintenance_floor = 16U * 1024U * 1024U;
		return limits;
	}
	static ResourceLimits databaseDefaults() {
        auto limits = ResourceLimits{256U * 1024U * 1024U,
            {192U * 1024U * 1024U, 64U * 1024U * 1024U, 128U * 1024U * 1024U, 32U * 1024U * 1024U, 0U}};
        // The process floor alone cannot protect a database whose non-maintenance
        // allocations fill its smaller total allowance.
        limits.protected_maintenance_floor = 16U * 1024U * 1024U;
        return limits;
	}
	explicit ProcessResourceDomains(ResourceLimits process = processDefaults(), ResourceLimits database = databaseDefaults(),
		size_t maximumDomains = 64U, size_t maximumQueries = 64U)
		: process_(std::make_shared<ProcessResourceBudget>(process, maximumDomains)),
		  database_limits_(database), maximum_domains_(maximumDomains), maximum_queries_(maximumQueries) {
		database_limits_.validate();
		if (database.total > process.total || maximumDomains > slots_.size() || maximumQueries == 0U)
			throw std::invalid_argument("invalid process resource domain limits");
	}
	std::shared_ptr<DatabaseResourceBudget> domain(std::string identity) {
		if (identity.empty() || identity.size() > 256U) throw std::invalid_argument("invalid resource domain identity");
		std::unique_lock lock(mutex_);
		size_t empty = maximum_domains_;
		for (size_t i = 0U; i < maximum_domains_; ++i) {
			if (auto existing = slots_[i].domain.lock()) {
				if (slots_[i].identity == identity) return existing;
			} else empty = i;
		}
		if (empty == maximum_domains_) { lock.unlock(); throw ResourceAdmissionDenied(); }
		auto result = std::make_shared<DatabaseResourceBudget>(process_, database_limits_, maximum_queries_);
		slots_[empty].identity = std::move(identity); slots_[empty].domain = result;
        slots_[empty].file_limit.reset();
		return result;
	}
    // Called before entering metadata publication. Aliases share one prepaid
    // cache allowance, and the registry retains it weakly to avoid an owner cycle.
    std::shared_ptr<PrepaidMemoryResource> cacheGrant(const std::shared_ptr<DatabaseResourceBudget> &domain) {
        std::unique_lock lock(mutex_);
        for (size_t i = 0U; i < maximum_domains_; ++i) {
            if (slots_[i].domain.lock() != domain || !domain) continue;
            if (auto existing = slots_[i].cache.lock()) return existing;
            const size_t capacity = database_limits_.classes[static_cast<size_t>(ResourceClass::RCK_CACHE)];
            if (capacity == 0U) throw ResourceAdmissionDenied();
            auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_CACHE, capacity);
            auto result = std::make_shared<PrepaidMemoryResource>(std::move(account), capacity, std::pmr::new_delete_resource());
            slots_[i].cache = result;
            return result;
        }
        throw std::invalid_argument("cache domain does not belong to this registry");
    }
    // Serialize native workspace ownership before metadata publication, without
    // holding registry/resource locks while waiting. Fixed slots keep the gate
    // stable; the caller's domain owner prevents slot reuse during a write.
    std::unique_lock<std::mutex> lockWriteWorkspace(const std::shared_ptr<DatabaseResourceBudget> &domain) {
        std::unique_lock registry(mutex_);
        for (size_t i = 0U; i < maximum_domains_; ++i) {
            if (!domain || slots_[i].domain.lock() != domain) continue;
            auto *gate = &slots_[i].write_workspace;
            registry.unlock();
            return std::unique_lock<std::mutex>(*gate);
        }
        throw std::invalid_argument("write workspace domain does not belong to this registry");
    }
    // Snapshot registration/control memory must not compete with decoded cache
    // payload. Prepay before publication so pin/reclamation paths use local
    // accounting only. This does not cover all maintenance workspaces.
    std::shared_ptr<PrepaidMemoryResource> controlGrant(const std::shared_ptr<DatabaseResourceBudget> &domain) {
        std::unique_lock lock(mutex_);
        for (size_t i = 0U; i < maximum_domains_; ++i) {
            if (slots_[i].domain.lock() != domain || !domain) continue;
            if (auto existing = slots_[i].control.lock()) return existing;
            const size_t capacity = (std::min)(size_t{1024U * 1024U},
                database_limits_.classes[static_cast<size_t>(ResourceClass::RCK_MAINTENANCE)]);
            if (capacity == 0U) throw ResourceAdmissionDenied();
            auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_MAINTENANCE, capacity);
            auto result = std::make_shared<PrepaidMemoryResource>(std::move(account), capacity, std::pmr::new_delete_resource());
            slots_[i].control = result;
            return result;
        }
        throw std::invalid_argument("control domain does not belong to this registry");
    }
    static constexpr size_t LOGICAL_ID_CAPABILITY_GRANT_BYTES = 256U * 1024U;
    // Volatile capability-table metadata belongs to WRITE. Admit its shared
    // finite grant before entering any metadata publication path.
    std::shared_ptr<PrepaidMemoryResource> logicalIdGrant(const std::shared_ptr<DatabaseResourceBudget> &domain) {
        std::unique_lock lock(mutex_);
        for (size_t i = 0U; i < maximum_domains_; ++i) {
            if (slots_[i].domain.lock() != domain || !domain) continue;
            if (auto existing = slots_[i].logical_ids.lock()) return existing;
            const size_t capacity = (std::min)(LOGICAL_ID_CAPABILITY_GRANT_BYTES,
                database_limits_.classes[static_cast<size_t>(ResourceClass::RCK_WRITE)]);
            if (capacity == 0U) throw ResourceAdmissionDenied();
            auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, capacity);
            auto result = std::make_shared<PrepaidMemoryResource>(std::move(account), capacity, std::pmr::new_delete_resource());
            slots_[i].logical_ids = result;
            return result;
        }
        throw std::invalid_argument("logical-ID domain does not belong to this registry");
    }
    // Writable aliases must use the same immutable absolute file ceiling.
    // Reject conflicting live configurations before opening a metadata writer;
    // read-only handles do not impose or raise writable storage limits.
    std::shared_ptr<const uint64_t> fileLimit(const std::shared_ptr<DatabaseResourceBudget> &domain,
                                            uint64_t maximumBytes) {
        if (maximumBytes == 0U || maximumBytes >= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            throw std::invalid_argument("database file limit must be finite and addressable");
        std::unique_lock lock(mutex_);
        for (size_t i = 0U; i < maximum_domains_; ++i) {
            if (slots_[i].domain.lock() != domain || !domain) continue;
            if (auto existing = slots_[i].file_limit.lock()) {
                if (*existing != maximumBytes) throw std::invalid_argument("writable database aliases have conflicting file byte limits");
                return existing;
            }
            auto result = std::make_shared<const uint64_t>(maximumBytes);
            slots_[i].file_limit = result;
            return result;
        }
        throw std::invalid_argument("file-limit domain does not belong to this registry");
    }
	[[nodiscard]] ResourceUsage usage() const { return process_->usage(); }
	static std::shared_ptr<ProcessResourceDomains> defaultOwner() {
		static auto owner = std::make_shared<ProcessResourceDomains>();
		return owner;
	}
private:
	struct Slot { std::string identity; std::weak_ptr<DatabaseResourceBudget> domain; std::weak_ptr<PrepaidMemoryResource> cache; std::weak_ptr<PrepaidMemoryResource> control; std::weak_ptr<PrepaidMemoryResource> logical_ids; std::weak_ptr<const uint64_t> file_limit; std::mutex write_workspace; };
	std::shared_ptr<ProcessResourceBudget> process_;
	ResourceLimits database_limits_;
	size_t maximum_domains_, maximum_queries_;
	std::mutex mutex_;
	std::array<Slot, 64U> slots_;
};
} // namespace graph::query::execution::detail
