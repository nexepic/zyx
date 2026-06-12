#include "graph/query/planner/RelationshipPropertyPredicatePlanner.hpp"

#include <utility>

namespace graph::query::planner {
namespace {

std::optional<PropertyValue> literalToValue(const expressions::Expression *expression) {
	const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(expression);
	if (literal == nullptr) {
		return std::nullopt;
	}
	if (literal->isNull()) {
		return PropertyValue{};
	}
	if (literal->isBoolean()) {
		return PropertyValue(literal->getBooleanValue());
	}
	if (literal->isInteger()) {
		return PropertyValue(literal->getIntegerValue());
	}
	if (literal->isDouble()) {
		return PropertyValue(literal->getDoubleValue());
	}
	if (literal->isString()) {
		return PropertyValue(literal->getStringValue());
	}
	return std::nullopt;
}

std::optional<std::string> extractRelationshipPropertyName(const expressions::Expression *expression,
                                                           const std::string &relationshipVariable) {
	const auto *var = dynamic_cast<const expressions::VariableReferenceExpression *>(expression);
	if (var == nullptr) {
		return std::nullopt;
	}
	if (var->hasProperty()) {
		if (var->getVariableName() != relationshipVariable) {
			return std::nullopt;
		}
		return var->getPropertyName();
	}

	const std::string prefix = relationshipVariable + ".";
	if (var->getVariableName().rfind(prefix, 0) != 0 ||
	    var->getVariableName().size() <= prefix.size()) {
		return std::nullopt;
	}
	return var->getVariableName().substr(prefix.size());
}

std::optional<execution::VectorPredicateOp> toVectorPredicateOp(expressions::BinaryOperatorType op,
                                                                 bool propertyOnLeft) {
	using BinaryOperatorType = expressions::BinaryOperatorType;
	using VectorPredicateOp = execution::VectorPredicateOp;
	switch (op) {
		case BinaryOperatorType::BOP_EQUAL:
			return VectorPredicateOp::VPO_EQ;
		case BinaryOperatorType::BOP_NOT_EQUAL:
			return VectorPredicateOp::VPO_NE;
		case BinaryOperatorType::BOP_LESS:
			return propertyOnLeft ? VectorPredicateOp::VPO_LT : VectorPredicateOp::VPO_GT;
		case BinaryOperatorType::BOP_LESS_EQUAL:
			return propertyOnLeft ? VectorPredicateOp::VPO_LE : VectorPredicateOp::VPO_GE;
		case BinaryOperatorType::BOP_GREATER:
			return propertyOnLeft ? VectorPredicateOp::VPO_GT : VectorPredicateOp::VPO_LT;
		case BinaryOperatorType::BOP_GREATER_EQUAL:
			return propertyOnLeft ? VectorPredicateOp::VPO_GE : VectorPredicateOp::VPO_LE;
		default:
			return std::nullopt;
	}
}

bool appendPredicate(const std::string &relationshipVariable,
                     const std::string &propertyName,
                     execution::VectorPredicateOp op,
                     const PropertyValue &value,
                     RelationshipPropertyPredicatePlan &plan) {
	if (op == execution::VectorPredicateOp::VPO_EQ) {
		const auto existing = plan.equalityProperties.find(propertyName);
		if (existing != plan.equalityProperties.end() && existing->second != value) {
			return false;
		}
	}

	execution::VectorizedPropertyPredicate predicate;
	predicate.variable = relationshipVariable;
	predicate.propertyKey = propertyName;
	predicate.op = op;
	predicate.value = value;
	plan.predicates.push_back(std::move(predicate));
	if (op == execution::VectorPredicateOp::VPO_EQ) {
		plan.equalityProperties[propertyName] = value;
	}
	return true;
}

bool appendExpressionPredicate(const expressions::Expression *expression,
                               const std::string &relationshipVariable,
                               RelationshipPropertyPredicatePlan &plan) {
	const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(expression);
	if (binary == nullptr) {
		return false;
	}

	if (binary->getOperator() == expressions::BinaryOperatorType::BOP_AND) {
		return appendExpressionPredicate(binary->getLeft(), relationshipVariable, plan) &&
		       appendExpressionPredicate(binary->getRight(), relationshipVariable, plan);
	}

	auto value = literalToValue(binary->getRight());
	if (auto propertyName = extractRelationshipPropertyName(binary->getLeft(), relationshipVariable);
	    propertyName.has_value() && value.has_value()) {
		auto op = toVectorPredicateOp(binary->getOperator(), true);
		if (!op.has_value()) {
			return false;
		}
		return appendPredicate(relationshipVariable, *propertyName, *op, *value, plan);
	}

	value = literalToValue(binary->getLeft());
	if (auto propertyName = extractRelationshipPropertyName(binary->getRight(), relationshipVariable);
	    propertyName.has_value() && value.has_value()) {
		auto op = toVectorPredicateOp(binary->getOperator(), false);
		if (!op.has_value()) {
			return false;
		}
		return appendPredicate(relationshipVariable, *propertyName, *op, *value, plan);
	}

	return false;
}

} // namespace

std::optional<RelationshipPropertyPredicatePlan>
buildRelationshipPropertyPredicatePlan(
		const std::string &relationshipVariable,
		const std::unordered_map<std::string, PropertyValue> &structuralProperties,
		const std::shared_ptr<expressions::Expression> &filterPredicate) {
	RelationshipPropertyPredicatePlan plan;
	plan.predicates.reserve(structuralProperties.size());
	for (const auto &[key, value] : structuralProperties) {
		if (!appendPredicate(relationshipVariable, key, execution::VectorPredicateOp::VPO_EQ, value, plan)) {
			return std::nullopt;
		}
	}

	if (filterPredicate == nullptr) {
		return plan;
	}
	if (!appendExpressionPredicate(filterPredicate.get(), relationshipVariable, plan)) {
		return std::nullopt;
	}
	return plan;
}

} // namespace graph::query::planner
