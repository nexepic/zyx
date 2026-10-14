#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include <variant>

#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/BatchIdentity.hpp"

namespace graph::query::execution::detail {

enum class NumericColumnKind { NCK_DYNAMIC, NCK_NULL, NCK_INTEGER, NCK_DOUBLE };

// Owns converted primitive buffers while borrowing the already charged source.
// Source and budget outlive this task-local owner; no views escape the batch.
class PrimitiveNumericColumns final {
	struct Column {
		NumericColumnKind kind = NumericColumnKind::NCK_DYNAMIC;
		bool prepared = false;
		const std::vector<PropertyValue> *source = nullptr;
		std::pmr::vector<int64_t> integers;
		std::pmr::vector<double> doubles;
		std::pmr::vector<uint8_t> present;
		explicit Column(std::pmr::memory_resource *memory) : integers(memory), doubles(memory), present(memory) {}
		Column(const Column &) = delete;
		Column &operator=(const Column &) = delete;
		Column(Column &&) noexcept = default;
		Column &operator=(Column &&) = default;
	};

public:
	explicit PrimitiveNumericColumns(MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: memory_(budget, upstream), columns_(&memory_), budget_(&budget) {
		if (!upstream) throw std::invalid_argument("numeric columns require an allocator");
	}
	PrimitiveNumericColumns(const PrimitiveNumericColumns &) = delete;
	PrimitiveNumericColumns &operator=(const PrimitiveNumericColumns &) = delete;
	PrimitiveNumericColumns(PrimitiveNumericColumns &&) = delete;
	PrimitiveNumericColumns &operator=(PrimitiveNumericColumns &&) = delete;

	template <typename Guard>
	[[nodiscard]] bool tryPrepare(std::span<const std::vector<PropertyValue>> source,
		std::span<const size_t> requested, Guard &&guard) {
		if (prepared_ || !columns_.empty()) throw std::logic_error("numeric columns were already prepared");
		const size_t rows = source.empty() ? 0U : source.front().size();
		if (rows > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("numeric batch exceeds its uint32 row limit");
		for (const auto &column : source) {
			guard();
			if (column.size() != rows) throw std::invalid_argument("numeric column matrix has inconsistent rows");
		}
		for (size_t ordinal : requested) {
			if (ordinal != std::numeric_limits<size_t>::max() && ordinal >= source.size())
				throw std::out_of_range("numeric column binding exceeds its source");
		}
		identity_.advance();
		try {
			columns_.reserve(source.size());
			for (const auto &column : source) {
				columns_.emplace_back(&memory_);
				columns_.back().source = &column;
			}
		} catch (const MaterializedByteLimitExceeded &) {
			std::pmr::vector<Column>(&memory_).swap(columns_);
			return false;
		}
		for (size_t ordinal : requested) {
			if (ordinal == std::numeric_limits<size_t>::max() || columns_[ordinal].prepared) continue;
			auto &target = columns_[ordinal];
			target.prepared = true;
			NumericColumnKind kind = NumericColumnKind::NCK_NULL;
			for (const auto &value : *target.source) {
				guard();
				if (value.getType() == PropertyType::NULL_TYPE) continue;
				const auto next = value.getType() == PropertyType::INTEGER ? NumericColumnKind::NCK_INTEGER :
					value.getType() == PropertyType::DOUBLE ? NumericColumnKind::NCK_DOUBLE : NumericColumnKind::NCK_DYNAMIC;
				if (next == NumericColumnKind::NCK_DYNAMIC || (kind != NumericColumnKind::NCK_NULL && kind != next)) {
					kind = NumericColumnKind::NCK_DYNAMIC;
					break;
				}
				kind = next;
			}
			if (kind == NumericColumnKind::NCK_NULL) { target.kind = kind; continue; }
			if (kind == NumericColumnKind::NCK_DYNAMIC) continue;
			Column staged(&memory_);
			staged.kind = kind;
			staged.prepared = true;
			staged.source = target.source;
			try {
				staged.present.reserve(rows);
				if (kind == NumericColumnKind::NCK_INTEGER) staged.integers.reserve(rows);
				else staged.doubles.reserve(rows);
			} catch (const MaterializedByteLimitExceeded &) {
				// No aggregate state has changed. The temporary refunds partial
				// reservations before this field falls back to dynamic values.
				continue;
			}
			for (const auto &value : *target.source) {
				guard(); // Cancellation must not be caught as optional budget pressure.
				const bool present = value.getType() != PropertyType::NULL_TYPE;
				staged.present.push_back(present);
				if (kind == NumericColumnKind::NCK_INTEGER)
					staged.integers.push_back(present ? std::get<int64_t>(value.getVariant()) : int64_t{});
				else staged.doubles.push_back(present ? std::get<double>(value.getVariant()) : 0.0);
			}
			target = std::move(staged);
		}
		rows_ = rows;
		prepared_ = true; // Publish handles only after all guarded preparation succeeds.
		return true;
	}

	[[nodiscard]] NumericColumnKind kind(size_t column) const { return columns_.at(column).kind; }
	[[nodiscard]] size_t size() const noexcept { return rows_; }
	[[nodiscard]] size_t columnCount() const noexcept { return columns_.size(); }
	[[nodiscard]] const MaterializedByteBudget *budgetDomain() const noexcept { return budget_; }
	[[nodiscard]] BatchRowId borrowVersion() const {
		if (!prepared_) throw std::logic_error("numeric batch has no published rows");
		return identity_.row(0U);
	}
	void validateBorrowVersion(BatchRowId version) const {
		identity_.validate(version);
		if (!prepared_) throw std::logic_error("numeric batch has no published rows");
	}
	using BorrowedCell = std::variant<std::monostate, int64_t, double, const PropertyValue *>;
	[[nodiscard]] BorrowedCell cell(size_t column, BatchRowId descriptor) const {
		validateBorrowVersion(descriptor);
		if (descriptor.index >= rows_) throw std::out_of_range("numeric cell exceeds its published rows");
		const auto &values = columns_.at(column); const size_t row = descriptor.index;
		if (values.source->at(row).getType() == PropertyType::NULL_TYPE) return std::monostate{};
		if (values.kind == NumericColumnKind::NCK_INTEGER) return values.integers.at(row);
		if (values.kind == NumericColumnKind::NCK_DOUBLE) return values.doubles.at(row);
		return &values.source->at(row); // Declared owning-PropertyValue source adapter.
	}
	void release() noexcept {
		identity_.invalidate();
		prepared_ = false;
		rows_ = 0U;
		std::pmr::vector<Column>(&memory_).swap(columns_);
	}
	[[nodiscard]] BatchRowId rowId(size_t index) const {
		if (!prepared_) throw std::logic_error("numeric batch has no published rows");
		if (index >= rows_) throw std::out_of_range("numeric batch row exceeds its published rows");
		return identity_.row(static_cast<uint32_t>(index));
	}
	[[nodiscard]] size_t typedColumnCount() const {
		size_t result = 0U;
		for (const auto &column : columns_) if (column.prepared && column.kind != NumericColumnKind::NCK_DYNAMIC) ++result;
		return result;
	}
	void update(AggregateAccumulator &accumulator, AggregateFunctionType function, size_t column, size_t row,
		const std::function<void(size_t)> &admit = [](size_t) {}) const {
		update(accumulator, function, column, rowId(row), admit);
	}
	void update(AggregateAccumulator &accumulator, AggregateFunctionType function, size_t column, BatchRowId descriptor,
		const std::function<void(size_t)> &admit = [](size_t) {}) const {
		identity_.validate(descriptor);
		if (!prepared_) throw std::logic_error("numeric batch has no published rows");
		if (descriptor.index >= rows_) throw std::out_of_range("numeric batch row exceeds its published rows");
		const size_t row = descriptor.index;
		if (function != AggregateFunctionType::AGG_COUNT && function != AggregateFunctionType::AGG_SUM &&
			function != AggregateFunctionType::AGG_AVG && function != AggregateFunctionType::AGG_MIN &&
			function != AggregateFunctionType::AGG_MAX && function != AggregateFunctionType::AGG_STDEV &&
			function != AggregateFunctionType::AGG_STDEVP) throw std::invalid_argument("unsupported numeric column aggregate");
		const auto &values = columns_.at(column);
		if (row >= values.source->size()) throw std::out_of_range("numeric column row exceeds its source");
		if (values.kind == NumericColumnKind::NCK_DYNAMIC) {
			if (function == AggregateFunctionType::AGG_MIN || function == AggregateFunctionType::AGG_MAX)
				accumulator.updateAdmitted(values.source->at(row), admit);
			else accumulator.update(values.source->at(row));
			return;
		}
		if (values.kind == NumericColumnKind::NCK_NULL || !values.present.at(row)) return;
		// The runtime factory supplies exactly these non-distinct accumulator types.
		if (function == AggregateFunctionType::AGG_COUNT) static_cast<CountAccumulator &>(accumulator).CountAccumulator::updateRow();
		else if (function == AggregateFunctionType::AGG_SUM) {
			auto &sum = static_cast<SumAccumulator &>(accumulator);
			if (values.kind == NumericColumnKind::NCK_INTEGER) sum.updateInteger(values.integers.at(row));
			else sum.updateDouble(values.doubles.at(row));
		} else if (function == AggregateFunctionType::AGG_AVG) {
			auto &average = static_cast<AvgAccumulator &>(accumulator);
			if (values.kind == NumericColumnKind::NCK_INTEGER) average.updateInteger(values.integers.at(row));
			else average.updateDouble(values.doubles.at(row));
		} else if (function == AggregateFunctionType::AGG_STDEV || function == AggregateFunctionType::AGG_STDEVP) {
			auto &deviation = static_cast<StDevAccumulator &>(accumulator);
			if (values.kind == NumericColumnKind::NCK_INTEGER) deviation.updateInteger(values.integers.at(row));
			else deviation.updateDouble(values.doubles.at(row));
		} else if (function == AggregateFunctionType::AGG_MIN) {
			auto &minimum = static_cast<MinAccumulator &>(accumulator);
			if (values.kind == NumericColumnKind::NCK_INTEGER) minimum.updateInteger(values.integers.at(row));
			else minimum.updateDouble(values.doubles.at(row));
		} else {
			auto &maximum = static_cast<MaxAccumulator &>(accumulator);
			if (values.kind == NumericColumnKind::NCK_INTEGER) maximum.updateInteger(values.integers.at(row));
			else maximum.updateDouble(values.doubles.at(row));
		}
	}

private:
	BatchIdentity identity_;
	size_t rows_ = 0U;
	bool prepared_ = false;
	BudgetMemoryResource memory_;
	std::pmr::vector<Column> columns_;
	const MaterializedByteBudget *budget_;
};

} // namespace graph::query::execution::detail
