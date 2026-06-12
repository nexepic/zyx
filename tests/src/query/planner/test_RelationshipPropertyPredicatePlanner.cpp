#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/planner/RelationshipPropertyPredicatePlanner.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::planner;

namespace {

using BinaryOperatorType = expressions::BinaryOperatorType;
using VectorPredicateOp = execution::VectorPredicateOp;

std::unique_ptr<expressions::Expression> property(const std::string &name) {
	return std::make_unique<expressions::VariableReferenceExpression>("r", name);
}

std::unique_ptr<expressions::Expression> dottedProperty(const std::string &name) {
	return std::make_unique<expressions::VariableReferenceExpression>("r." + name);
}

std::unique_ptr<expressions::Expression> literal(int64_t value) {
	return std::make_unique<expressions::LiteralExpression>(value);
}

std::unique_ptr<expressions::Expression> comparison(std::unique_ptr<expressions::Expression> left,
                                                    BinaryOperatorType op,
                                                    std::unique_ptr<expressions::Expression> right) {
	return std::make_unique<expressions::BinaryOpExpression>(std::move(left), op, std::move(right));
}

std::shared_ptr<expressions::Expression> shared(std::unique_ptr<expressions::Expression> expression) {
	return std::shared_ptr<expressions::Expression>(std::move(expression));
}

const execution::VectorizedPropertyPredicate *findPredicate(
		const std::vector<execution::VectorizedPropertyPredicate> &predicates,
		const std::string &key) {
	for (const auto &predicate : predicates) {
		if (predicate.propertyKey == key) {
			return &predicate;
		}
	}
	return nullptr;
}

} // namespace

TEST(RelationshipPropertyPredicatePlannerTest, ConvertsStructuralPropertiesIntoEqualityPredicates) {
	const std::unordered_map<std::string, PropertyValue> properties = {
			{"weight", PropertyValue(int64_t{7})},
			{"tier", PropertyValue("gold")},
	};

	auto plan = buildRelationshipPropertyPredicatePlan("r", properties, nullptr);

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->equalityProperties.at("weight"), PropertyValue(int64_t{7}));
	EXPECT_EQ(plan->equalityProperties.at("tier"), PropertyValue("gold"));
	EXPECT_EQ(plan->predicates.size(), 2U);
	ASSERT_NE(findPredicate(plan->predicates, "weight"), nullptr);
	EXPECT_EQ(findPredicate(plan->predicates, "weight")->op, VectorPredicateOp::VPO_EQ);
	ASSERT_NE(findPredicate(plan->predicates, "tier"), nullptr);
	EXPECT_EQ(findPredicate(plan->predicates, "tier")->variable, "r");
}

TEST(RelationshipPropertyPredicatePlannerTest, ExtractsLiteralEqualityValuesFromFilterPredicates) {
	auto left = comparison(
			property("active"),
			BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(true));
	auto middle = comparison(
			property("missing"),
			BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>());
	auto right = comparison(
			std::make_unique<expressions::LiteralExpression>(std::string("gold")),
			BinaryOperatorType::BOP_EQUAL,
			dottedProperty("tier"));
	auto conjunction = comparison(
			std::move(left),
			BinaryOperatorType::BOP_AND,
			comparison(std::move(middle), BinaryOperatorType::BOP_AND, std::move(right)));

	auto plan = buildRelationshipPropertyPredicatePlan("r", {}, shared(std::move(conjunction)));

	ASSERT_TRUE(plan.has_value());
	EXPECT_EQ(plan->equalityProperties.at("active"), PropertyValue(true));
	EXPECT_EQ(plan->equalityProperties.at("missing"), PropertyValue{});
	EXPECT_EQ(plan->equalityProperties.at("tier"), PropertyValue("gold"));
	EXPECT_EQ(plan->predicates.size(), 3U);
}

TEST(RelationshipPropertyPredicatePlannerTest, MapsPropertyOnLeftComparisonOperators) {
	const std::vector<std::pair<BinaryOperatorType, VectorPredicateOp>> cases = {
			{BinaryOperatorType::BOP_NOT_EQUAL, VectorPredicateOp::VPO_NE},
			{BinaryOperatorType::BOP_LESS, VectorPredicateOp::VPO_LT},
			{BinaryOperatorType::BOP_LESS_EQUAL, VectorPredicateOp::VPO_LE},
			{BinaryOperatorType::BOP_GREATER, VectorPredicateOp::VPO_GT},
			{BinaryOperatorType::BOP_GREATER_EQUAL, VectorPredicateOp::VPO_GE},
	};

	for (const auto &[op, expected] : cases) {
		auto plan = buildRelationshipPropertyPredicatePlan(
				"r",
				{},
				shared(comparison(property("score"), op, literal(10))));

		ASSERT_TRUE(plan.has_value());
		ASSERT_EQ(plan->predicates.size(), 1U);
		EXPECT_EQ(plan->predicates[0].op, expected);
		EXPECT_TRUE(plan->equalityProperties.empty());
	}
}

TEST(RelationshipPropertyPredicatePlannerTest, MapsLiteralOnLeftComparisonOperators) {
	const std::vector<std::pair<BinaryOperatorType, VectorPredicateOp>> cases = {
			{BinaryOperatorType::BOP_LESS, VectorPredicateOp::VPO_GT},
			{BinaryOperatorType::BOP_LESS_EQUAL, VectorPredicateOp::VPO_GE},
			{BinaryOperatorType::BOP_GREATER, VectorPredicateOp::VPO_LT},
			{BinaryOperatorType::BOP_GREATER_EQUAL, VectorPredicateOp::VPO_LE},
	};

	for (const auto &[op, expected] : cases) {
		auto plan = buildRelationshipPropertyPredicatePlan(
				"r",
				{},
				shared(comparison(literal(10), op, dottedProperty("score"))));

		ASSERT_TRUE(plan.has_value());
		ASSERT_EQ(plan->predicates.size(), 1U);
		EXPECT_EQ(plan->predicates[0].propertyKey, "score");
		EXPECT_EQ(plan->predicates[0].op, expected);
	}
}

TEST(RelationshipPropertyPredicatePlannerTest, RejectsUnsupportedOrAmbiguousFilterPredicates) {
	auto nonBinaryPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			std::make_shared<expressions::VariableReferenceExpression>("r", "weight"));
	EXPECT_FALSE(nonBinaryPlan.has_value());

	auto arithmeticPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(property("weight"), BinaryOperatorType::BOP_ADD, literal(1))));
	EXPECT_FALSE(arithmeticPlan.has_value());

	auto variableValuePlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(property("weight"),
			                  BinaryOperatorType::BOP_EQUAL,
			                  std::make_unique<expressions::VariableReferenceExpression>("other"))));
	EXPECT_FALSE(variableValuePlan.has_value());

	auto otherRelationshipPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(
					std::make_unique<expressions::VariableReferenceExpression>("x", "weight"),
					BinaryOperatorType::BOP_EQUAL,
					literal(1))));
	EXPECT_FALSE(otherRelationshipPlan.has_value());
}

TEST(RelationshipPropertyPredicatePlannerTest, RejectsMalformedDottedNamesAndShortCircuitsInvalidConjunctions) {
	auto emptyDottedNamePlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(literal(1),
			                  BinaryOperatorType::BOP_EQUAL,
			                  std::make_unique<expressions::VariableReferenceExpression>("r."))));
	EXPECT_FALSE(emptyDottedNamePlan.has_value());

	auto invalidLeft = comparison(
			std::make_unique<expressions::VariableReferenceExpression>("x", "weight"),
			BinaryOperatorType::BOP_EQUAL,
			literal(1));
	auto validRight = comparison(property("weight"), BinaryOperatorType::BOP_EQUAL, literal(1));
	auto plan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(std::move(invalidLeft), BinaryOperatorType::BOP_AND, std::move(validRight))));
	EXPECT_FALSE(plan.has_value());
}

TEST(RelationshipPropertyPredicatePlannerTest, RejectsConflictingEqualityPredicates) {
	const std::unordered_map<std::string, PropertyValue> structuralProperties = {
			{"weight", PropertyValue(int64_t{1})},
	};

	auto consistentPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			structuralProperties,
			shared(comparison(property("weight"), BinaryOperatorType::BOP_EQUAL, literal(1))));
	ASSERT_TRUE(consistentPlan.has_value());
	EXPECT_EQ(consistentPlan->equalityProperties.at("weight"), PropertyValue(int64_t{1}));

	auto conflictingStructuralPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			structuralProperties,
			shared(comparison(property("weight"), BinaryOperatorType::BOP_EQUAL, literal(2))));
	EXPECT_FALSE(conflictingStructuralPlan.has_value());

	auto conflictingFilterPlan = buildRelationshipPropertyPredicatePlan(
			"r",
			{},
			shared(comparison(
					comparison(property("weight"), BinaryOperatorType::BOP_EQUAL, literal(1)),
					BinaryOperatorType::BOP_AND,
					comparison(property("weight"), BinaryOperatorType::BOP_EQUAL, literal(2)))));
	EXPECT_FALSE(conflictingFilterPlan.has_value());
}
