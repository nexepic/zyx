#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace graph::query::execution::detail {

enum class ResourceClass : size_t { RCK_QUERY, RCK_CACHE, RCK_WRITE, RCK_MAINTENANCE, RCK_CODE, RCK_COUNT };
inline constexpr size_t RESOURCE_CLASS_COUNT = static_cast<size_t>(ResourceClass::RCK_COUNT);
struct ResourceLimits {
	size_t total;
	std::array<size_t, RESOURCE_CLASS_COUNT> classes;
	size_t protected_maintenance_floor = 0U;
	static ResourceLimits uniform(size_t bytes) {
		ResourceLimits limits{bytes, {}};
		limits.classes.fill(bytes);
		return limits;
	}
	void validate() const {
		if (total == 0U) throw std::invalid_argument("resource total must be positive");
		if (protected_maintenance_floor > total ||
			protected_maintenance_floor > classes[static_cast<size_t>(ResourceClass::RCK_MAINTENANCE)])
			throw std::invalid_argument("protected maintenance floor exceeds resource limits");
		for (size_t maximum : classes)
			if (maximum > total) throw std::invalid_argument("resource class limit exceeds total");
	}
};
struct ResourceUsage {
	size_t total = 0U;
	std::array<size_t, RESOURCE_CLASS_COUNT> classes{};
};
class ResourceAdmissionDenied final : public std::runtime_error {
public:
	ResourceAdmissionDenied() : std::runtime_error("hierarchical resource admission denied") {}
};
class ResourceAccount;
class ResourceLease;

// Explicit process owner; not an implicit singleton. One mutex makes admission
// atomic across process/database/account totals. No resource waits, callbacks,
// allocation, eviction or I/O execute while it is held.
class ProcessResourceBudget final {
public:
	explicit ProcessResourceBudget(ResourceLimits limits, size_t maximumDatabases = 64U)
		: limits_(limits), maximum_databases_(maximumDatabases) {
		limits_.validate();
		if (maximumDatabases == 0U) throw std::invalid_argument("resource database slots must be positive");
	}
	ProcessResourceBudget(const ProcessResourceBudget &) = delete;
	ProcessResourceBudget &operator=(const ProcessResourceBudget &) = delete;
	[[nodiscard]] ResourceUsage usage() const { std::lock_guard lock(mutex_); return usage_; }
private:
	friend class DatabaseResourceBudget;
	friend class ResourceAccount;
	friend class ResourceLease;
	const ResourceLimits limits_;
	mutable std::mutex mutex_;
	ResourceUsage usage_;
	const size_t maximum_databases_;
	size_t databases_ = 0U;
};
class DatabaseResourceBudget final {
public:
	DatabaseResourceBudget(ProcessResourceBudget &process, ResourceLimits limits, size_t maximumQueries = 64U)
		: process_(process), limits_(limits), maximum_queries_(maximumQueries) {
		limits_.validate();
		if (limits_.total > process.limits_.total) throw std::invalid_argument("database resource total exceeds process total");
		if (maximumQueries == 0U) throw std::invalid_argument("resource query slots must be positive");
		std::unique_lock lock(process_.mutex_);
		if (process_.databases_ == process_.maximum_databases_) {
			lock.unlock();throw ResourceAdmissionDenied();
		}
		++process_.databases_;
	}
	DatabaseResourceBudget(std::shared_ptr<ProcessResourceBudget> process, ResourceLimits limits, size_t maximumQueries = 64U)
		: DatabaseResourceBudget(requireOwner(process), limits, maximumQueries) { process_owner_ = std::move(process); }
	~DatabaseResourceBudget() { std::lock_guard lock(process_.mutex_); --process_.databases_; }
	DatabaseResourceBudget(const DatabaseResourceBudget &) = delete;
	DatabaseResourceBudget &operator=(const DatabaseResourceBudget &) = delete;
	[[nodiscard]] ResourceUsage usage() const { std::lock_guard lock(process_.mutex_); return usage_; }
	[[nodiscard]] size_t maximum() const noexcept { return limits_.total; }
    [[nodiscard]] size_t classMaximum(ResourceClass kind) const {
        const auto index = static_cast<size_t>(kind);
        if (index >= RESOURCE_CLASS_COUNT) throw std::invalid_argument("unknown resource class");
        return limits_.classes[index];
    }
private:
	friend class ResourceAccount;
	friend class ResourceLease;
	static ProcessResourceBudget &requireOwner(const std::shared_ptr<ProcessResourceBudget> &owner) {
		if (!owner) throw std::invalid_argument("database resource requires process owner");
		return *owner;
	}
	std::shared_ptr<ProcessResourceBudget> process_owner_;
	ProcessResourceBudget &process_;
	const ResourceLimits limits_;
	ResourceUsage usage_;
	const size_t maximum_queries_;
	size_t queries_ = 0U;
};

// Every lease shares one account quota: splitting a lease cannot multiply the
// query allowance. Borrowed owners require process > database > account > leases;
// shared-owner constructors retain that hierarchy for escaped output leases.
class ResourceAccount final {
public:
	ResourceAccount(DatabaseResourceBudget &database, ResourceClass kind, size_t maximum)
		: database_(database), kind_(static_cast<size_t>(kind)), maximum_(maximum) {
		if (kind_ >= RESOURCE_CLASS_COUNT || maximum == 0U || maximum > database.limits_.total)
			throw std::invalid_argument("resource account requires a valid class and finite quota");
		if (kind_ == static_cast<size_t>(ResourceClass::RCK_QUERY)) {
			std::unique_lock lock(database_.process_.mutex_);
			if (database_.queries_ == database_.maximum_queries_) {
				lock.unlock();throw ResourceAdmissionDenied();
			}
			++database_.queries_;
		}
	}
	ResourceAccount(std::shared_ptr<DatabaseResourceBudget> database, ResourceClass kind, size_t maximum)
		: ResourceAccount(requireOwner(database), kind, maximum) { database_owner_ = std::move(database); }
	~ResourceAccount() {
		if (kind_ == static_cast<size_t>(ResourceClass::RCK_QUERY)) {
			std::lock_guard lock(database_.process_.mutex_); --database_.queries_;
		}
	}
	ResourceAccount(const ResourceAccount &) = delete;
	ResourceAccount &operator=(const ResourceAccount &) = delete;
	[[nodiscard]] size_t used() const { std::lock_guard lock(database_.process_.mutex_); return used_; }
	[[nodiscard]] size_t maximum() const noexcept { return maximum_; }
	[[nodiscard]] ResourceClass resourceClass() const noexcept { return static_cast<ResourceClass>(kind_); }
	[[nodiscard]] bool belongsTo(const DatabaseResourceBudget &database) const noexcept { return &database_ == &database; }
	// A snapshot for bounded operator workspace decisions, not a reservation.
	// Actual admission rechecks the same limits under the process mutex.
	[[nodiscard]] size_t available() const {
		std::lock_guard lock(database_.process_.mutex_);
		return availableLocked();
	}
private:
	friend class ResourceLease;
	[[nodiscard]] size_t availableLocked() const noexcept {
		const auto &process = database_.process_;
		size_t remaining = (std::min)({maximum_ - used_,
			database_.limits_.total - database_.usage_.total,
			process.limits_.total - process.usage_.total,
			database_.limits_.classes[kind_] - database_.usage_.classes[kind_],
			process.limits_.classes[kind_] - process.usage_.classes[kind_]});
		const size_t maintenance = static_cast<size_t>(ResourceClass::RCK_MAINTENANCE);
		if (kind_ != maintenance) {
			remaining = (std::min)({remaining,
				database_.limits_.total - database_.limits_.protected_maintenance_floor -
					(database_.usage_.total - database_.usage_.classes[maintenance]),
				process.limits_.total - process.limits_.protected_maintenance_floor -
					(process.usage_.total - process.usage_.classes[maintenance])});
		}
		return remaining;
	}
	static DatabaseResourceBudget &requireOwner(const std::shared_ptr<DatabaseResourceBudget> &owner) {
		if (!owner) throw std::invalid_argument("resource account requires database owner");
		return *owner;
	}
	std::shared_ptr<DatabaseResourceBudget> database_owner_;
	DatabaseResourceBudget &database_;
	const size_t kind_, maximum_;
	size_t used_ = 0U;
};

// A lease is confined to its allocator/task; independent leases may be used
// concurrently. Destroy held allocations before refunding their lease.
class ResourceLease final {
public:
	explicit ResourceLease(ResourceAccount &account) noexcept : account_(&account) {}
	explicit ResourceLease(std::shared_ptr<ResourceAccount> account) : owner_(std::move(account)), account_(owner_.get()) {
		if (!account_) throw std::invalid_argument("resource lease requires account owner");
	}
	~ResourceLease() { reset(); }
	ResourceLease(const ResourceLease &) = delete;
	ResourceLease &operator=(const ResourceLease &) = delete;
	ResourceLease(ResourceLease &&other) noexcept
		: owner_(std::move(other.owner_)), account_(std::exchange(other.account_, nullptr)), bytes_(std::exchange(other.bytes_, 0U)) {}
	ResourceLease &operator=(ResourceLease &&other) {
		if (this != &other) { reset(); owner_ = std::move(other.owner_); account_ = std::exchange(other.account_, nullptr); bytes_ = std::exchange(other.bytes_, 0U); }
		return *this;
	}
	[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
	[[nodiscard]] size_t available() const { return account_ ? account_->available() : 0U; }
	// A deep copy retains a separate allocation. Admit its complete charge
	// before copying any payload; sharing the original lease would undercount.
	[[nodiscard]] ResourceLease duplicate() const {
		if (!account_) throw std::logic_error("moved resource lease has no account");
		ResourceLease result(*account_);
		result.owner_ = owner_;
		result.set(bytes_);
		return result;
	}
	[[nodiscard]] bool trySet(size_t next) {
		if (!account_) {
			if (next != 0U) throw std::logic_error("moved resource lease has no account");
			return true;
		}
		auto &database = account_->database_;
		auto &process = database.process_;
		std::lock_guard lock(process.mutex_);
		const size_t kind = account_->kind_;
		if (next > bytes_) {
			const size_t delta = next - bytes_;
			if (delta > account_->availableLocked()) return false;
			account_->used_ += delta;
			database.usage_.total += delta; database.usage_.classes[kind] += delta;
			process.usage_.total += delta; process.usage_.classes[kind] += delta;
		} else {
			const size_t delta = bytes_ - next;
			account_->used_ -= delta;
			database.usage_.total -= delta; database.usage_.classes[kind] -= delta;
			process.usage_.total -= delta; process.usage_.classes[kind] -= delta;
		}
		bytes_ = next;
		return true;
	}
	void set(size_t next) { if (!trySet(next)) throw ResourceAdmissionDenied(); }
	void reset() { (void) trySet(0U); }
	[[nodiscard]] ResourceLease splitOff(size_t bytes) {
		if (bytes > bytes_ || !account_) throw std::logic_error("invalid resource lease transfer");
		ResourceLease result(*account_);
		result.owner_ = owner_;
		result.bytes_ = bytes;
		bytes_ -= bytes;
		return result;
	}
	void absorb(ResourceLease &&other) {
		if (this == &other) return;
		if (!account_ || account_ != other.account_) throw std::logic_error("resource transfer crosses account ownership");
		if (other.bytes_ > (std::numeric_limits<size_t>::max)() - bytes_) throw std::overflow_error("resource transfer overflow");
		if (!owner_) owner_ = other.owner_;
		bytes_ += std::exchange(other.bytes_, 0U);
	}
private:
	std::shared_ptr<ResourceAccount> owner_;
	ResourceAccount *account_;
	size_t bytes_ = 0U;
};

class CoordinatedMemoryResource final : public std::pmr::memory_resource {
public:
	CoordinatedMemoryResource(ResourceAccount &account, std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: lease_(account), upstream_(upstream) {
		if (!upstream) throw std::invalid_argument("coordinated allocator requires an upstream resource");
	}
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		const size_t previous = lease_.bytes();
		if (bytes > (std::numeric_limits<size_t>::max)() - previous) throw std::overflow_error("coordinated allocation size overflow");
		lease_.set(previous + bytes);
		try { return upstream_->allocate(bytes, alignment); }
		catch (...) { lease_.set(previous); throw; }
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		upstream_->deallocate(pointer, bytes, alignment);
		lease_.set(lease_.bytes() - bytes);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
	ResourceLease lease_;
	std::pmr::memory_resource *upstream_;
};
} // namespace graph::query::execution::detail
