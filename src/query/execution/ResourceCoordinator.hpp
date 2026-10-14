#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace graph::query::execution::detail {

enum class ResourceClass : size_t { RCK_QUERY, RCK_CACHE, RCK_WRITE, RCK_MAINTENANCE, RCK_CODE, RCK_COUNT };
inline constexpr size_t RESOURCE_CLASS_COUNT = static_cast<size_t>(ResourceClass::RCK_COUNT);
struct ResourceLimits {
	size_t total;
	std::array<size_t, RESOURCE_CLASS_COUNT> classes;
	static ResourceLimits uniform(size_t bytes) {
		ResourceLimits limits{bytes, {}};
		limits.classes.fill(bytes);
		return limits;
	}
	void validate() const {
		if (total == 0U) throw std::invalid_argument("resource total must be positive");
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
	explicit ProcessResourceBudget(ResourceLimits limits) : limits_(limits) { limits_.validate(); }
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
};
class DatabaseResourceBudget final {
public:
	DatabaseResourceBudget(ProcessResourceBudget &process, ResourceLimits limits) : process_(process), limits_(limits) {
		limits_.validate();
		if (limits_.total > process.limits_.total) throw std::invalid_argument("database resource total exceeds process total");
	}
	DatabaseResourceBudget(const DatabaseResourceBudget &) = delete;
	DatabaseResourceBudget &operator=(const DatabaseResourceBudget &) = delete;
	[[nodiscard]] ResourceUsage usage() const { std::lock_guard lock(process_.mutex_); return usage_; }
private:
	friend class ResourceAccount;
	friend class ResourceLease;
	ProcessResourceBudget &process_;
	const ResourceLimits limits_;
	ResourceUsage usage_;
};

// Every lease shares one account quota: splitting a lease cannot multiply the
// query allowance. Lifetime order is process > database > account > leases.
class ResourceAccount final {
public:
	ResourceAccount(DatabaseResourceBudget &database, ResourceClass kind, size_t maximum)
		: database_(database), kind_(static_cast<size_t>(kind)), maximum_(maximum) {
		if (kind_ >= RESOURCE_CLASS_COUNT || maximum == 0U || maximum > database.limits_.total)
			throw std::invalid_argument("resource account requires a valid class and finite quota");
	}
	ResourceAccount(const ResourceAccount &) = delete;
	ResourceAccount &operator=(const ResourceAccount &) = delete;
	[[nodiscard]] size_t used() const { std::lock_guard lock(database_.process_.mutex_); return used_; }
private:
	friend class ResourceLease;
	DatabaseResourceBudget &database_;
	const size_t kind_, maximum_;
	size_t used_ = 0U;
};

// A lease is confined to its allocator/task; independent leases may be used
// concurrently. Destroy held allocations before refunding their lease.
class ResourceLease final {
public:
	explicit ResourceLease(ResourceAccount &account) noexcept : account_(&account) {}
	~ResourceLease() { reset(); }
	ResourceLease(const ResourceLease &) = delete;
	ResourceLease &operator=(const ResourceLease &) = delete;
	ResourceLease(ResourceLease &&other) noexcept
		: account_(std::exchange(other.account_, nullptr)), bytes_(std::exchange(other.bytes_, 0U)) {}
	ResourceLease &operator=(ResourceLease &&other) {
		if (this != &other) { reset(); account_ = std::exchange(other.account_, nullptr); bytes_ = std::exchange(other.bytes_, 0U); }
		return *this;
	}
	[[nodiscard]] size_t bytes() const noexcept { return bytes_; }
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
			if (delta > account_->maximum_ - account_->used_ ||
				delta > database.limits_.total - database.usage_.total ||
				delta > process.limits_.total - process.usage_.total ||
				delta > database.limits_.classes[kind] - database.usage_.classes[kind] ||
				delta > process.limits_.classes[kind] - process.usage_.classes[kind]) return false;
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
		result.bytes_ = bytes;
		bytes_ -= bytes;
		return result;
	}
	void absorb(ResourceLease &&other) {
		if (this == &other) return;
		if (!account_ || account_ != other.account_) throw std::logic_error("resource transfer crosses account ownership");
		if (other.bytes_ > (std::numeric_limits<size_t>::max)() - bytes_) throw std::overflow_error("resource transfer overflow");
		bytes_ += std::exchange(other.bytes_, 0U);
	}
private:
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
