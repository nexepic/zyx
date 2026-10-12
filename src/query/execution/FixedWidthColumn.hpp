#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {

enum class ColumnCellState : uint8_t {
	CCS_MISSING,
	CCS_NULL,
	CCS_VALUE,
};

// Task-local, owning columns. The budget outlives the column; all views expire
// on mutation. clear() reuses charged capacity; release() returns it.
template <typename T>
class FixedWidthColumn final {
	static_assert(std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>);

public:
	using StorageType = std::conditional_t<std::is_same_v<T, bool>, uint8_t, T>;

	FixedWidthColumn(size_t maximumRows, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: maximum_rows_(maximumRows), memory_(budget, upstream), values_(&memory_), states_(&memory_) {
		if (maximumRows == 0U) throw std::invalid_argument("typed column requires a positive row limit");
		if (upstream == nullptr) throw std::invalid_argument("typed column requires an allocation resource");
	}
	FixedWidthColumn(const FixedWidthColumn &) = delete;
	FixedWidthColumn &operator=(const FixedWidthColumn &) = delete;
	FixedWidthColumn(FixedWidthColumn &&) = delete;
	FixedWidthColumn &operator=(FixedWidthColumn &&) = delete;

	[[nodiscard]] size_t size() const noexcept { return values_.size(); }
	[[nodiscard]] size_t maximumRows() const noexcept { return maximum_rows_; }
	[[nodiscard]] std::span<const StorageType> values() const noexcept { return values_; }
	[[nodiscard]] std::span<const ColumnCellState> states() const noexcept { return states_; }
	[[nodiscard]] ColumnCellState state(size_t index) const { return states_.at(index); }
	[[nodiscard]] T value(size_t index) const {
		if (states_.at(index) != ColumnCellState::CCS_VALUE)
			throw std::logic_error("typed column cell has no value");
		return static_cast<T>(values_[index]);
	}
	// Batch producers know their bounded cardinality before appending. Reserve
	// once to avoid repeated growth and overlapping old/new allocations.
	void reserve(size_t rows) {
		if (rows > maximum_rows_) throw std::out_of_range("typed column reservation exceeds its row limit");
		values_.reserve(rows);
		states_.reserve(rows);
	}

	void append(T value) { appendCell(static_cast<StorageType>(value), ColumnCellState::CCS_VALUE); }
	void appendNull() { appendCell(StorageType{}, ColumnCellState::CCS_NULL); }
	void appendMissing() { appendCell(StorageType{}, ColumnCellState::CCS_MISSING); }
	// A rectangular batch can roll back columns appended before a later column
	// failed. Capacity stays allocated and charged until release/destruction.
	void truncate(size_t count) {
		if (count > size()) throw std::out_of_range("typed column cannot grow through truncation");
		values_.resize(count);
		states_.resize(count);
	}
	void clear() noexcept { values_.clear(); states_.clear(); }
	void release() noexcept {
		std::pmr::vector<StorageType> values(&memory_);
		std::pmr::vector<ColumnCellState> states(&memory_);
		values_.swap(values);
		states_.swap(states);
	}

private:
	void appendCell(StorageType value, ColumnCellState state) {
		if (size() == maximum_rows_) throw std::runtime_error("typed column exceeded its row limit");
		// Admit actual allocations, including old/new overlap. Both reserves
		// precede size changes, so a failed append preserves every existing cell.
		// Successfully reserved capacity may remain charged after a later failure.
		const size_t needed = size() + 1U;
		if (values_.capacity() < needed) values_.reserve(nextCapacity(values_.capacity()));
		if (states_.capacity() < needed) states_.reserve(nextCapacity(states_.capacity()));
		values_.push_back(value);
		states_.push_back(state);
	}
	[[nodiscard]] size_t nextCapacity(size_t current) const noexcept {
		if (current == 0U) return 1U;
		return current > maximum_rows_ / 2U ? maximum_rows_ : current * 2U;
	}

	size_t maximum_rows_;
	BudgetMemoryResource memory_;
	std::pmr::vector<StorageType> values_;
	std::pmr::vector<ColumnCellState> states_;
};

} // namespace graph::query::execution::detail
