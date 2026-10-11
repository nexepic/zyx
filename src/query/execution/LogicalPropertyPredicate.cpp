/**
 * @file LogicalPropertyPredicate.cpp
 * @brief Typed predicate evaluation used by the native logical graph runtime.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalPropertyPredicate.hpp"

namespace graph::query::execution {

namespace {

[[nodiscard]] bool comparePropertyValues(
	const PropertyValue& actual,
	const LogicalPropertyPredicate& predicate
) {
	switch (predicate.op) {
		case LogicalPropertyPredicateOp::LPP_EQ:
			return actual == predicate.value;
		case LogicalPropertyPredicateOp::LPP_NE:
			return actual != predicate.value;
		case LogicalPropertyPredicateOp::LPP_LT:
			return actual < predicate.value;
		case LogicalPropertyPredicateOp::LPP_LE:
			return actual <= predicate.value;
		case LogicalPropertyPredicateOp::LPP_GT:
			return actual > predicate.value;
		case LogicalPropertyPredicateOp::LPP_GE:
			return actual >= predicate.value;
		case LogicalPropertyPredicateOp::LPP_RANGE_CLOSED:
			return predicate.upper_value.has_value() &&
				actual >= predicate.value && actual <= *predicate.upper_value;
	}
	return false;
}

} // namespace

bool evaluateLogicalPropertyPredicate(
	const std::optional<PropertyValue>& actual,
	const LogicalPropertyPredicate& predicate
) {
	if (!actual.has_value()) return false;
	return comparePropertyValues(*actual, predicate);
}

} // namespace graph::query::execution
