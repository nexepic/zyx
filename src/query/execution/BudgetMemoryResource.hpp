#pragma once
#include <memory_resource>
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::execution::detail {

// Containers must be destroyed before this resource and the shared budget.
// Charges allocator requests, including node/bucket overlap during rehash.
class BudgetMemoryResource final : public std::pmr::memory_resource {
public:
	explicit BudgetMemoryResource(MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: reservation_(&budget), upstream_(upstream) {}
	[[nodiscard]] size_t used() const noexcept { return reservation_.bytes(); }
private:
	void *do_allocate(std::size_t bytes, std::size_t alignment) override {
		const auto old = reservation_.bytes();
		if (bytes > std::numeric_limits<std::size_t>::max() - old)
			throw std::overflow_error("execution allocator byte count overflowed");
		reservation_.set(old + bytes);
		try { return upstream_->allocate(bytes, alignment); }
		catch (...) { reservation_.set(old); throw; }
	}
	void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
		upstream_->deallocate(pointer, bytes, alignment);
		reservation_.set(reservation_.bytes() - bytes);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
	MaterializedByteReservation reservation_;
	std::pmr::memory_resource *upstream_;
};

} // namespace graph::query::execution::detail
