#pragma once

#include <tuple>
#include <utility>

#include "query/execution/FixedWidthColumn.hpp"
#include "query/execution/BatchIdentity.hpp"

namespace graph::query::execution::detail {

template <typename T>
class TypedCell final {
	static_assert(std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>);
public:
	[[nodiscard]] static TypedCell withValue(T value) noexcept { return {value, ColumnCellState::CCS_VALUE}; }
	[[nodiscard]] static TypedCell null() noexcept { return {T{}, ColumnCellState::CCS_NULL}; }
	[[nodiscard]] static TypedCell missing() noexcept { return {T{}, ColumnCellState::CCS_MISSING}; }
	[[nodiscard]] ColumnCellState state() const noexcept { return state_; }
	[[nodiscard]] T value() const {
		if (state_ != ColumnCellState::CCS_VALUE) throw std::logic_error("typed cell has no value");
		return value_;
	}

private:
	TypedCell(T value, ColumnCellState state) noexcept : value_(value), state_(state) {}
	T value_;
	ColumnCellState state_;
};

// Inline recursive storage constructs nonmovable allocator-owning columns at
// their final addresses, without uncharged per-column heap allocations.
template <typename... Ts>
struct FixedWidthColumns;

template <>
struct FixedWidthColumns<> {
	FixedWidthColumns(size_t, MaterializedByteBudget &, std::pmr::memory_resource *) {}
	void truncate(size_t) {}
	void clear() noexcept {}
	void release() noexcept {}
};

template <typename T, typename... Rest>
struct FixedWidthColumns<T, Rest...> {
	FixedWidthColumns(size_t maximumRows, MaterializedByteBudget &budget, std::pmr::memory_resource *upstream)
		: head(maximumRows, budget, upstream), tail(maximumRows, budget, upstream) {}
	template <size_t Index>
	[[nodiscard]] auto &get() noexcept {
		if constexpr (Index == 0U) return head;
		else return tail.template get<Index - 1U>();
	}
	template <size_t Index>
	[[nodiscard]] const auto &get() const noexcept {
		if constexpr (Index == 0U) return head;
		else return tail.template get<Index - 1U>();
	}
	void truncate(size_t count) { head.truncate(count); tail.truncate(count); }
	void clear() noexcept { head.clear(); tail.clear(); }
	void release() noexcept { head.release(); tail.release(); }
	FixedWidthColumn<T> head;
	FixedWidthColumns<Rest...> tail;
};

template <typename... Ts>
class FixedWidthBatch final {
	static_assert(sizeof...(Ts) > 0U, "typed batch requires at least one column");

public:
	FixedWidthBatch(size_t maximumRows, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: maximum_rows_(checkedMaximumRows(maximumRows)), columns_(maximumRows, budget, upstream) {}
	FixedWidthBatch(const FixedWidthBatch &) = delete;
	FixedWidthBatch &operator=(const FixedWidthBatch &) = delete;
	FixedWidthBatch(FixedWidthBatch &&) = delete;
	FixedWidthBatch &operator=(FixedWidthBatch &&) = delete;

	[[nodiscard]] size_t size() const noexcept { return rows_; }
	[[nodiscard]] size_t maximumRows() const noexcept { return maximum_rows_; }
	[[nodiscard]] static constexpr size_t columnCount() noexcept { return sizeof...(Ts); }
	[[nodiscard]] BatchRowId rowId(size_t index) const {
		if (index >= rows_) throw std::out_of_range("batch row descriptor is outside its published rows");
		return identity_.row(static_cast<uint32_t>(index));
	}
	template <size_t Index>
	[[nodiscard]] auto cell(BatchRowId row) const {
		identity_.validate(row);
		if (row.index >= rows_) throw std::out_of_range("batch row descriptor is outside its published rows");
		using T = std::tuple_element_t<Index, std::tuple<Ts...>>;
		const auto &values = column<Index>();
		switch (values.state(row.index)) {
			case ColumnCellState::CCS_VALUE: return TypedCell<T>::withValue(values.value(row.index));
			case ColumnCellState::CCS_NULL: return TypedCell<T>::null();
			case ColumnCellState::CCS_MISSING: return TypedCell<T>::missing();
		}
		throw std::logic_error("typed batch has an invalid cell state");
	}
	template <size_t Index>
	[[nodiscard]] const auto &column() const noexcept {
		static_assert(Index < sizeof...(Ts));
		return columns_.template get<Index>();
	}

	void append(TypedCell<Ts>... cells) {
		if (rows_ == maximum_rows_) throw std::runtime_error("typed batch exceeded its row limit");
		identity_.advance(); // A failed reserve can invalidate previously borrowed buffers.
		try {
			appendColumns(std::tuple{cells...}, std::index_sequence_for<Ts...>{});
		} catch (...) {
			columns_.truncate(rows_);
			throw;
		}
		++rows_; // Publish only after every column succeeded.
	}
	void clear() noexcept { identity_.invalidate(); columns_.clear(); rows_ = 0U; }
	void release() noexcept { identity_.invalidate(); columns_.release(); rows_ = 0U; }

private:
	static size_t checkedMaximumRows(size_t rows) {
		if (rows == 0U || rows > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("typed batch requires a positive uint32 row limit");
		return rows;
	}
	template <size_t Index, typename T>
	void appendColumn(const TypedCell<T> &cell) {
		auto &column = columns_.template get<Index>();
		switch (cell.state()) {
			case ColumnCellState::CCS_VALUE: column.append(cell.value()); break;
			case ColumnCellState::CCS_NULL: column.appendNull(); break;
			case ColumnCellState::CCS_MISSING: column.appendMissing(); break;
		}
	}
	template <size_t... Indices>
	void appendColumns(const std::tuple<TypedCell<Ts>...> &cells, std::index_sequence<Indices...>) {
		(appendColumn<Indices>(std::get<Indices>(cells)), ...);
	}

	size_t maximum_rows_;
	size_t rows_ = 0U;
	BatchIdentity identity_;
	FixedWidthColumns<Ts...> columns_;
};

} // namespace graph::query::execution::detail
