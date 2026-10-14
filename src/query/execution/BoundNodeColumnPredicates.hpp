#pragma once

#include <algorithm>
#include <span>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "query/execution/PrimitiveColumnFilter.hpp"
#include "query/execution/BatchEnvelope.hpp"

namespace graph::query::execution::detail {

struct ColumnFilterStats {
	size_t typed_columns = 0U;
	size_t scalar_columns = 0U;
};

// Snapshot-local column ordinals, not durable property tokens. Expected values
// borrow the immutable logical plan or query-local resolved parameters. The
// producer authenticates the entire matrix before this task-local consumer runs.
class BoundNodeColumnPredicates final {
public:
	BoundNodeColumnPredicates(const logical::LogicalNodeScan &scan,
		std::span<const std::pair<std::string, PropertyValue>> parameters,
		std::span<const std::string> keys, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: column_count_(keys.size()), memory_(budget, upstream), predicates_(&memory_) {
		if (upstream == nullptr) throw std::invalid_argument("bound column predicates require an allocator");
		const auto add = [&](std::string_view key, ColumnPredicateOp operation, const PropertyValue *value) {
			if (key.empty()) throw std::invalid_argument("bound column predicate has an empty key");
			const auto position = std::ranges::find(keys, key);
			if (position == keys.end()) throw std::invalid_argument("bound column predicate is missing its input column");
			predicates_.push_back({static_cast<size_t>(position - keys.begin()), operation, value});
		};
		const auto equalities = [&](const auto &values) {
			for (const auto &[key, value] : values) add(key, ColumnPredicateOp::CPO_EQUAL, &value);
		};
		equalities(scan.getPropertyPredicates());
		equalities(parameters);
		for (const auto &range : scan.getRangePredicates()) {
			// An unbounded range still requires a non-NULL property.
			add(range.key, ColumnPredicateOp::CPO_NON_NULL, nullptr);
			if (range.minValue.getType() != PropertyType::NULL_TYPE)
				add(range.key, range.minInclusive ? ColumnPredicateOp::CPO_GREATER_EQUAL : ColumnPredicateOp::CPO_GREATER,
					&range.minValue);
			if (range.maxValue.getType() != PropertyType::NULL_TYPE)
				add(range.key, range.maxInclusive ? ColumnPredicateOp::CPO_LESS_EQUAL : ColumnPredicateOp::CPO_LESS,
					&range.maxValue);
		}
		if (const auto &composite = scan.getCompositeEquality(); composite) {
			if (composite->keys.empty() || composite->keys.size() != composite->values.size())
				throw std::invalid_argument("bound composite predicate has an invalid shape");
			for (size_t index = 0U; index < composite->keys.size(); ++index)
				add(composite->keys[index], ColumnPredicateOp::CPO_EQUAL, &composite->values[index]);
		}
		std::ranges::sort(predicates_, {}, &ColumnPredicate::column);
	}
	BoundNodeColumnPredicates(const BoundNodeColumnPredicates &) = delete;
	BoundNodeColumnPredicates &operator=(const BoundNodeColumnPredicates &) = delete;
	BoundNodeColumnPredicates(BoundNodeColumnPredicates &&) = delete;
	BoundNodeColumnPredicates &operator=(BoundNodeColumnPredicates &&) = delete;

	template <typename Guard>
	[[nodiscard]] ColumnFilterStats filter(std::span<const std::vector<PropertyValue>> columns,
		SelectionVector &selection, MaterializedByteBudget &budget, Guard &&guard,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource()) const {
		if (columns.size() != column_count_) throw std::invalid_argument("bound column matrix has a different width");
		for (const auto &column : columns) {
			guard();
			if (column.size() != selection.size()) throw std::invalid_argument("bound column matrix has a different cardinality");
		}
		ColumnFilterStats stats;
		size_t begin = 0U;
		while (begin < predicates_.size()) {
			size_t end = begin + 1U;
			const size_t ordinal = predicates_[begin].column;
			while (end < predicates_.size() && predicates_[end].column == ordinal) ++end;
			const auto conditions = std::span<const ColumnPredicate>(predicates_).subspan(begin, end - begin);
			if (tryFilterPrimitiveColumn(columns[ordinal], conditions, selection, budget, guard, upstream)) {
				++stats.typed_columns;
			} else {
				++stats.scalar_columns;
				for (size_t index = 0U; index < selection.size(); ++index) {
					guard();
					if (!selection.includes(index)) continue;
					for (const auto &condition : conditions) {
						if (!columnPredicateMatches(columns[ordinal][index], condition)) {
							selection.reject(index);
							break;
						}
					}
				}
			}
			begin = end;
		}
		return stats;
	}

	// Consume the same authenticated batch descriptors used by scan/project
	// and aggregate. Primitive cells stay primitive; mixed values borrow the
	// already charged source rather than allocate a second row/value copy.
	template<class Guard>
	[[nodiscard]] ColumnFilterStats filter(const BatchEnvelope &batch, const BoundColumnLayout &layout,
		SelectionVector &selection, Guard guard) const {
		if (batch.size() != selection.size() || layout.size() < column_count_)
			throw std::invalid_argument("bound filter envelope changed its shape");
		ColumnFilterStats stats;
		size_t previousColumn = SIZE_MAX;
		for (const auto &predicate : predicates_) {
			guard(); const auto column = layout.column(predicate.column);
			if (predicate.column != previousColumn) {
				const auto kind = batch.columnKind(column);
				if (kind == BatchColumnKind::BCK_INTEGER || kind == BatchColumnKind::BCK_DOUBLE || kind == BatchColumnKind::BCK_BOOLEAN)
					++stats.typed_columns;
				else ++stats.scalar_columns;
				previousColumn = predicate.column;
			}
			for (size_t index = 0U; index < selection.size(); ++index) {
				guard(); if (!selection.includes(index)) continue;
				const auto cell = batch.cell(column, batch.rowId(index));
				bool matches = false;
				if (cell.state == ColumnCellState::CCS_VALUE) matches = std::visit([&](auto value) -> bool {
					using T = decltype(value);
					if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>)
						return columnPredicateMatches(PropertyValue(value), predicate);
					else if constexpr (std::is_same_v<T, const PropertyValue *>)
						return columnPredicateMatches(*value, predicate);
					else throw std::invalid_argument("predicate envelope has an unsupported value source");
				}, cell.data);
				if (!matches) selection.reject(index);
			}
		}
		return stats;
	}

private:
	size_t column_count_;
	BudgetMemoryResource memory_;
	std::pmr::vector<ColumnPredicate> predicates_;
};

} // namespace graph::query::execution::detail
