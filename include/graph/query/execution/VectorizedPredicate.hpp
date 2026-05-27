#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/NodeColumnBatch.hpp"

namespace graph::query::execution {

	enum class VectorPredicateOp {
		VPO_EQ,
		VPO_NE,
		VPO_LT,
		VPO_LE,
		VPO_GT,
		VPO_GE,
		VPO_RANGE_CLOSED
	};

	struct VectorizedPropertyPredicate {
		std::string variable;
		std::string propertyKey;
		VectorPredicateOp op = VectorPredicateOp::VPO_EQ;
		PropertyValue value;
		std::optional<PropertyValue> upperValue;
	};

	[[nodiscard]] bool evaluatePredicateValue(const std::optional<PropertyValue> &actual,
	                                        const VectorizedPropertyPredicate &predicate);

	void applyPredicate(NodeColumnBatch &batch, const VectorizedPropertyPredicate &predicate);

	void applyPredicates(NodeColumnBatch &batch, const std::vector<VectorizedPropertyPredicate> &predicates);

} // namespace graph::query::execution
