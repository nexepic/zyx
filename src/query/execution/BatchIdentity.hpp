#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace graph::query::execution::detail {

struct BatchRowId {
	uint32_t index;
	uint64_t owner;
	uint64_t incarnation;
	bool operator==(const BatchRowId &) const = default;
};

// Numeric handles never borrow a buffer address. All mutations terminate old
// handles, including failed growth that may already have relocated a column.
class BatchIdentity final {
public:
	BatchIdentity() : owner_(nextOwner()) {}
	BatchIdentity(const BatchIdentity &) = delete;
	BatchIdentity &operator=(const BatchIdentity &) = delete;
	BatchIdentity(BatchIdentity &&) = delete;
	BatchIdentity &operator=(BatchIdentity &&) = delete;
	void advance() {
		if (incarnation_ == 0U || incarnation_ == (std::numeric_limits<uint64_t>::max)())
			throw std::overflow_error("batch incarnation exhausted");
		++incarnation_;
	}
	void invalidate() noexcept {
		// clear/release remain noexcept. Exhaustion permanently disables handles
		// and subsequent append, rather than allowing a generation to wrap.
		if (incarnation_ != 0U) {
			if (incarnation_ == (std::numeric_limits<uint64_t>::max)()) incarnation_ = 0U;
			else ++incarnation_;
		}
	}
	[[nodiscard]] BatchRowId row(uint32_t index) const {
		if (incarnation_ == 0U) throw std::overflow_error("batch incarnation exhausted");
		return {index, owner_, incarnation_};
	}
	void validate(BatchRowId row) const {
		if (incarnation_ == 0U || row.owner != owner_ || row.incarnation != incarnation_)
			throw std::invalid_argument("row descriptor belongs to another batch incarnation");
	}
private:
	static uint64_t nextOwner() {
		static std::atomic<uint64_t> next{1U};
		uint64_t value = next.load(std::memory_order_relaxed);
		for (;;) {
			if (value == (std::numeric_limits<uint64_t>::max)())
				throw std::overflow_error("batch owner identity exhausted");
			if (next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed)) return value;
		}
	}
	const uint64_t owner_;
	uint64_t incarnation_ = 1U;
};

} // namespace graph::query::execution::detail
