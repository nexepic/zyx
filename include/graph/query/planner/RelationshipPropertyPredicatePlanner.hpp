#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Property.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::planner {

struct RelationshipPropertyPredicatePlan {
	std::unordered_map<std::string, PropertyValue> equalityProperties;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
};

[[nodiscard]] std::optional<RelationshipPropertyPredicatePlan>
buildRelationshipPropertyPredicatePlan(
		const std::string &relationshipVariable,
		const std::unordered_map<std::string, PropertyValue> &structuralProperties,
		const std::shared_ptr<expressions::Expression> &filterPredicate);

} // namespace graph::query::planner
