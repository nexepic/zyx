#pragma once

#include <limits>
#include <span>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/SelectionVector.hpp"
#include "query/execution/FixedWidthColumn.hpp"

namespace graph::query::execution::detail {

enum class ColumnPredicateOp {
	CPO_EQUAL, CPO_NOT_EQUAL, CPO_LESS, CPO_GREATER,
	CPO_LESS_EQUAL, CPO_GREATER_EQUAL, CPO_NON_NULL
};

struct ColumnPredicate {
	size_t column;
	ColumnPredicateOp operation;
	const PropertyValue *expected;
};

// PropertyValue ordering is type-sensitive. Its <= and >= are negated <,
// including for NaN. Both the scalar fallback and typed loops retain that rule.
[[nodiscard]] inline bool columnPredicateMatches(const PropertyValue &actual, const ColumnPredicate &predicate) {
	if (actual.getType() == PropertyType::NULL_TYPE) return false;
	if (predicate.operation == ColumnPredicateOp::CPO_NON_NULL) return true;
	if (predicate.expected == nullptr) throw std::invalid_argument("column comparison has no expected value");
	if (predicate.expected->getType() == PropertyType::NULL_TYPE) return false;
	switch (predicate.operation) {
		case ColumnPredicateOp::CPO_EQUAL: return actual == *predicate.expected;
		case ColumnPredicateOp::CPO_NOT_EQUAL: return actual != *predicate.expected;
		case ColumnPredicateOp::CPO_LESS: return actual < *predicate.expected;
		case ColumnPredicateOp::CPO_GREATER: return actual > *predicate.expected;
		case ColumnPredicateOp::CPO_LESS_EQUAL: return actual <= *predicate.expected;
		case ColumnPredicateOp::CPO_GREATER_EQUAL: return actual >= *predicate.expected;
		case ColumnPredicateOp::CPO_NON_NULL: return true;
	}
	throw std::invalid_argument("invalid column comparison operation");
}

template <typename T>
struct TypedColumnPredicate {
	ColumnPredicateOp operation;
	T expected;
};

template <typename T, typename Guard>
[[nodiscard]] bool tryFilterPrimitiveColumnAs(std::span<const PropertyValue> source,
	std::span<const ColumnPredicate> predicates, SelectionVector &selection,
	MaterializedByteBudget &budget, Guard &guard, std::pmr::memory_resource *upstream) {
	for (const auto &value : source) {
		guard();
		if (value.getType() != PropertyType::NULL_TYPE && std::get_if<T>(&value.getVariant()) == nullptr) return false;
	}
	for (const auto &predicate : predicates) {
		if (predicate.operation != ColumnPredicateOp::CPO_NON_NULL &&
			std::get_if<T>(&predicate.expected->getVariant()) == nullptr) return false;
	}
	constexpr size_t cellBytes = sizeof(typename FixedWidthColumn<T>::StorageType) + sizeof(ColumnCellState);
	constexpr size_t constantBytes = sizeof(TypedColumnPredicate<T>);
	if (source.size() > budget.available() / cellBytes) return false;
	const size_t remaining = budget.available() - source.size() * cellBytes;
	if (predicates.size() > remaining / constantBytes) return false;

	FixedWidthColumn<T> column(source.size(), budget, upstream);
	BudgetMemoryResource constantsMemory(budget, upstream);
	std::pmr::vector<TypedColumnPredicate<T>> constants(&constantsMemory);
	try {
		column.reserve(source.size());
		constants.reserve(predicates.size());
		for (const auto &predicate : predicates) {
			constants.push_back({predicate.operation, predicate.operation == ColumnPredicateOp::CPO_NON_NULL
				? T{} : std::get<T>(predicate.expected->getVariant())});
		}
	} catch (const MaterializedByteLimitExceeded &) {
		// Decline only before mutating selection. PMR owners refund any capacity
		// retained after a partial reservation; bad_alloc and cancellation escape.
		return false;
	}
	for (const auto &value : source) {
		guard();
		if (value.getType() == PropertyType::NULL_TYPE) column.appendNull();
		else column.append(std::get<T>(value.getVariant()));
	}
	const auto values = column.values();
	const auto states = column.states();
	const auto filter = [&](T expected, const auto &compare) {
		for (size_t index = 0U; index < column.size(); ++index) {
			guard();
			if (selection.includes(index) && (states[index] != ColumnCellState::CCS_VALUE ||
				!compare(static_cast<T>(values[index]), expected))) selection.reject(index);
		}
	};
	for (const auto &predicate : constants) {
		switch (predicate.operation) {
			case ColumnPredicateOp::CPO_EQUAL: filter(predicate.expected, [](T a, T b) { return a == b; }); break;
			case ColumnPredicateOp::CPO_NOT_EQUAL: filter(predicate.expected, [](T a, T b) { return a != b; }); break;
			case ColumnPredicateOp::CPO_LESS: filter(predicate.expected, [](T a, T b) { return a < b; }); break;
			case ColumnPredicateOp::CPO_GREATER: filter(predicate.expected, [](T a, T b) { return b < a; }); break;
			case ColumnPredicateOp::CPO_LESS_EQUAL: filter(predicate.expected, [](T a, T b) { return !(b < a); }); break;
			case ColumnPredicateOp::CPO_GREATER_EQUAL: filter(predicate.expected, [](T a, T b) { return !(a < b); }); break;
			case ColumnPredicateOp::CPO_NON_NULL: filter(T{}, [](T, T) { return true; }); break;
		}
	}
	return true;
}

// The legacy column boundary maps missing and explicit NULL to monostate.
// Filter rejects either; future native typed producers can preserve both states.
template <typename Guard>
[[nodiscard]] bool tryFilterPrimitiveColumn(std::span<const PropertyValue> source,
	std::span<const ColumnPredicate> predicates, SelectionVector &selection,
	MaterializedByteBudget &budget, Guard &&guard,
	std::pmr::memory_resource *upstream = std::pmr::get_default_resource()) {
	if (source.size() != selection.size()) throw std::invalid_argument("column filter selection has a different cardinality");
	if (upstream == nullptr) throw std::invalid_argument("column filter requires an allocator");
	for (const auto &predicate : predicates) {
		if (predicate.operation != ColumnPredicateOp::CPO_NON_NULL && predicate.expected == nullptr)
			throw std::invalid_argument("column comparison has no expected value");
		switch (predicate.operation) {
			case ColumnPredicateOp::CPO_EQUAL: case ColumnPredicateOp::CPO_NOT_EQUAL:
			case ColumnPredicateOp::CPO_LESS: case ColumnPredicateOp::CPO_GREATER:
			case ColumnPredicateOp::CPO_LESS_EQUAL: case ColumnPredicateOp::CPO_GREATER_EQUAL:
			case ColumnPredicateOp::CPO_NON_NULL: break;
			default: throw std::invalid_argument("invalid column comparison operation");
		}
	}
	if (source.empty() || predicates.empty()) return true;
	for (const auto &value : source) {
		guard();
		if (value.getType() == PropertyType::NULL_TYPE) continue;
		if (std::holds_alternative<int64_t>(value.getVariant()))
			return tryFilterPrimitiveColumnAs<int64_t>(source, predicates, selection, budget, guard, upstream);
		if (std::holds_alternative<double>(value.getVariant()))
			return tryFilterPrimitiveColumnAs<double>(source, predicates, selection, budget, guard, upstream);
		if (std::holds_alternative<bool>(value.getVariant()))
			return tryFilterPrimitiveColumnAs<bool>(source, predicates, selection, budget, guard, upstream);
		return false;
	}
	selection.rejectAll();
	return true;
}

} // namespace graph::query::execution::detail
