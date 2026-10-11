/**
 * @file test_CypherStructuralExpressions.cpp
 * @brief Public QueryEngine contracts for structural Cypher expressions.
 *
 * These tests deliberately execute through Database rather than the retired
 * ExpressionEvaluator so accepted syntax has observable native behavior.
 */

#include "QueryTestFixture.hpp"

using namespace graph;

class CypherStructuralExpressionsTest : public QueryTestFixture {};

TEST_F(CypherStructuralExpressionsTest, EvaluatesCaseAndQuantifierExpressions) {
	const auto result = execute(
			"RETURN CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple_case, "
			"CASE WHEN null THEN 'wrong' WHEN true THEN 'right' ELSE 'other' END AS searched_case, "
			"all(value IN [1, 2, 3] WHERE value > 0) AS all_positive, "
			"any(value IN [1, 2, 3] WHERE value = 2) AS any_two, "
			"none(value IN [1, 2, 3] WHERE value < 0) AS none_negative, "
			"single(value IN [1, 2, 3] WHERE value = 2) AS exactly_one");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("simple_case").asPrimitive(), PropertyValue("two"));
	EXPECT_EQ(row.at("searched_case").asPrimitive(), PropertyValue("right"));
	EXPECT_EQ(row.at("all_positive").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_two").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("none_negative").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("exactly_one").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherStructuralExpressionsTest, PreservesCaseNullNoElseAndFallbackSemantics) {
	const auto result = execute(
			"RETURN CASE 1 WHEN null THEN 'invalid' WHEN 2 THEN 'invalid' ELSE 'fallback' END AS simple_fallback, "
			"CASE null WHEN null THEN 'invalid' ELSE 'null_fallback' END AS null_comparison, "
			"CASE 1 WHEN 2 THEN 'invalid' END AS simple_no_match, "
			"CASE WHEN null THEN 'invalid' END AS searched_null, "
			"CASE WHEN false THEN 'invalid' ELSE 'searched_fallback' END AS searched_fallback");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("simple_fallback").asPrimitive(), PropertyValue("fallback"));
	EXPECT_EQ(row.at("null_comparison").asPrimitive(), PropertyValue("null_fallback"));
	EXPECT_EQ(row.at("simple_no_match").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("searched_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("searched_fallback").asPrimitive(), PropertyValue("searched_fallback"));
}

TEST_F(CypherStructuralExpressionsTest, PreservesCaseFirstMatchAndTypedComparisonSemantics) {
	const auto result = execute(
			"RETURN CASE 1 WHEN 1 THEN 'first' WHEN 1 THEN 'second' ELSE 'fallback' END AS first_match, "
			"CASE 'b' WHEN 'a' THEN 1 WHEN 'b' THEN 2 ELSE 3 END AS string_match, "
			"CASE WHEN false THEN 'first' WHEN true THEN 'second' WHEN true THEN 'third' END AS searched_first");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("first_match").asPrimitive(), PropertyValue("first"));
	EXPECT_EQ(row.at("string_match").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("searched_first").asPrimitive(), PropertyValue("second"));
}

TEST_F(CypherStructuralExpressionsTest, PreservesEmptyAndNullQuantifierSemantics) {
	const auto result = execute(
			"RETURN all(value IN [] WHERE value > 0) AS all_empty, "
			"any(value IN [] WHERE value > 0) AS any_empty, "
			"none(value IN [] WHERE value > 0) AS none_empty, "
			"single(value IN [] WHERE value > 0) AS single_empty, "
			"all(value IN [null, 1] WHERE value > 0) AS all_null");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("all_empty").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_empty").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("none_empty").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("single_empty").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("all_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, PreservesThreeValuedQuantifierSemantics) {
	const auto result = execute(
			"RETURN "
			"all(value IN [false, null] WHERE value) AS all_false_wins, "
			"any(value IN [null, false] WHERE value) AS any_null, "
			"none(value IN [null, false] WHERE value) AS none_null, "
			"single(value IN [null, false] WHERE value) AS single_null, "
			"single(value IN [null, true] WHERE value) AS single_true, "
			"all(value IN null WHERE value) AS all_null_list");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("all_false_wins").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("any_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("none_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("single_null").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("single_true").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("all_null_list").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_THROW(static_cast<void>(execute("RETURN any(value IN [1, 2] WHERE 1) AS invalid_predicate")),
			 std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, PreservesThreeValuedInSemantics) {
	const auto result = execute(
			"RETURN 2 IN [null, 2] AS static_match, 3 IN [null, 2] AS static_unknown, "
			"3 IN [] AS static_miss, null IN [null, 2] AS null_value, "
			"'banana' IN ['apple', 'banana'] AS string_match, "
			"3.14 IN [1.5, 3.14, 2.7] AS floating_match, "
			"true IN [false, true] AS boolean_match, "
			"2 IN ['1', 2, 3.0] AS mixed_match, 2 IN [1, 2, 2] AS duplicate_match");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("static_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("static_unknown").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("static_miss").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("null_value").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("string_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("floating_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("boolean_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("mixed_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("duplicate_match").asPrimitive(), PropertyValue(true));

	(void) execute("CREATE (:Membership {candidates: [null, 2]})");
	const auto dynamicResult = execute(
			"MATCH (membership:Membership) "
			"RETURN 2 IN membership.candidates AS dynamic_match, "
			"3 IN membership.candidates AS dynamic_unknown");
	ASSERT_EQ(dynamicResult.rowCount(), 1U);
	const auto &dynamicRow = dynamicResult.getRows().front();
	EXPECT_EQ(dynamicRow.at("dynamic_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(dynamicRow.at("dynamic_unknown").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, PreservesNullForDynamicInList) {
	const auto result = execute("UNWIND [null] AS candidates RETURN 1 IN candidates AS membership");

	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("membership").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, PropagatesNullThroughListExpressions) {
	const auto result = execute(
			"RETURN null[0] AS item, null[..] AS slice, [value IN null | value] AS comprehension, "
			"reduce(total = 0, value IN null | total + value) AS reduced");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("item").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("slice").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("comprehension").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("reduced").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesListComprehensionScopeAndRejectsInvalidPredicates) {
	const auto result = execute(
			"RETURN [value IN [1, 2, 3, 4] WHERE value > 2 | value * 10] AS filtered, "
			"[value IN [1, 2, 3] | value + 1] AS mapped, "
			"[value IN [1, 2] WHERE false | value] AS empty");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("filtered").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30), PropertyValue(40)}));
	EXPECT_EQ(row.at("mapped").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3), PropertyValue(4)}));
	EXPECT_EQ(row.at("empty").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));

	EXPECT_THROW(static_cast<void>(execute("RETURN [value IN [1, 2] WHERE 1 | value] AS invalid_predicate")),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN [value IN 1 | value] AS invalid_source")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, RestoresOuterBindingsAfterScopedExpressions) {
	const auto result = execute(
			"UNWIND [10] AS value "
			"RETURN value AS outer_before, "
			"[value IN [1, 2] | [inner IN [value, value + 1] | value + inner]] AS nested, "
			"reduce(total = 0, value IN [1, 2] | total + value) AS reduced, "
			"any(value IN [1, 2] WHERE value = 2) AS any_two, value AS outer_after");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("outer_before").asPrimitive(), PropertyValue(10));
	const std::vector<PropertyValue> &nested = row.at("nested").asPrimitive().getList();
	ASSERT_EQ(nested.size(), 2U);
	EXPECT_EQ(nested[0].toString(), "[2, 3]");
	EXPECT_EQ(nested[1].toString(), "[4, 5]");
	EXPECT_EQ(row.at("reduced").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("any_two").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("outer_after").asPrimitive(), PropertyValue(10));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesDynamicListLiteralsInTheCurrentRowScope) {
	const auto result = execute(
			"UNWIND [1, 3] AS value "
			"RETURN [value, value + 1, [value * 2]] AS literal, value IN [value, value + 1] AS membership "
			"ORDER BY value");

	ASSERT_EQ(result.rowCount(), 2U);
	const auto &first = result.getRows().at(0U);
	EXPECT_EQ(first.at("literal").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2),
														 PropertyValue(std::vector<PropertyValue>{PropertyValue(2)})}));
	EXPECT_EQ(first.at("membership").asPrimitive(), PropertyValue(true));
	const auto &second = result.getRows().at(1U);
	EXPECT_EQ(second.at("literal").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(3), PropertyValue(4),
														 PropertyValue(std::vector<PropertyValue>{PropertyValue(6)})}));
	EXPECT_EQ(second.at("membership").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesDynamicInListMembersAgainstEachMatchedEntity) {
	(void) execute("CREATE (:DynamicMembership {x: 1, id: 7}), (:DynamicMembership {x: 2, id: 1})");

	const auto result = execute(
			"MATCH (node:DynamicMembership) "
			"RETURN node.x IN [node.id, 1] AS property_membership, "
			"node.x IN [node.x + 1, 2] AS arithmetic_membership "
			"ORDER BY node.x");

	ASSERT_EQ(result.rowCount(), 2U);
	const auto &first = result.getRows().at(0U);
	EXPECT_EQ(first.at("property_membership").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(first.at("arithmetic_membership").asPrimitive(), PropertyValue(false));
	const auto &second = result.getRows().at(1U);
	EXPECT_EQ(second.at("property_membership").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(second.at("arithmetic_membership").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesDynamicMapLiteralsInTheCurrentRowScope) {
	const auto result = execute(
			"UNWIND [5] AS value "
			"RETURN {value: value, next: value + 1, nested: {double: value * 2}, list: [value]} AS literal");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &map = result.getRows().front().at("literal").asPrimitive().getMap();
	EXPECT_EQ(map.at("value"), PropertyValue(5));
	EXPECT_EQ(map.at("next"), PropertyValue(6));
	EXPECT_EQ(map.at("nested"), PropertyValue(PropertyValue::MapType{{"double", PropertyValue(10)}}));
	EXPECT_EQ(map.at("list"), PropertyValue(std::vector<PropertyValue>{PropertyValue(5)}));
}

TEST_F(CypherStructuralExpressionsTest, ShortCircuitsCaseAndBooleanBranches) {
	const auto result = execute(
			"RETURN CASE WHEN true THEN 'safe' ELSE 1 / 0 END AS case_value, "
			"false AND (1 / 0 > 0) AS false_and, true OR (1 / 0 > 0) AS true_or, "
			"null AND false AS null_and_false, null OR true AS null_or_true");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("case_value").asPrimitive(), PropertyValue("safe"));
	EXPECT_EQ(row.at("false_and").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("true_or").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("null_and_false").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("null_or_true").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherStructuralExpressionsTest, PreservesThePublicThreeValuedBooleanTruthTable) {
	const auto result = execute(
			"RETURN false AND null AS and_false_wins, true AND null AS and_unknown, "
			"false OR null AS or_unknown, true OR null AS or_true_wins, "
			"true XOR false AS xor_true, true XOR true AS xor_false, "
			"null XOR false AS xor_unknown_left, false XOR null AS xor_unknown_right, "
			"true OR false XOR true AND false AS precedence");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("and_false_wins").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("and_unknown").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("or_unknown").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("or_true_wins").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("xor_true").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("xor_false").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("xor_unknown_left").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("xor_unknown_right").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("precedence").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesBetweenThroughThePublicQueryEngine) {
	(void) execute("CREATE (:BetweenValue {value: 1}), (:BetweenValue {value: 5}), "
				  "(:BetweenValue {value: 10}), (:BetweenValue {value: 15}), (:BetweenValue {value: 2.5})");

	const auto integerResult = execute(
			"MATCH (node:BetweenValue) WHERE node.value BETWEEN 5 AND 10 "
			"RETURN node.value AS value ORDER BY value ASC");
	ASSERT_EQ(integerResult.rowCount(), 2U);
	EXPECT_EQ(integerResult.getRows()[0].at("value").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(integerResult.getRows()[1].at("value").asPrimitive(), PropertyValue(10));

	const auto mixedNumericResult = execute(
			"MATCH (node:BetweenValue) WHERE node.value BETWEEN 2.0 AND 3.0 "
			"RETURN node.value AS value");
	ASSERT_EQ(mixedNumericResult.rowCount(), 1U);
	EXPECT_EQ(mixedNumericResult.getRows().front().at("value").asPrimitive(), PropertyValue(2.5));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesInfixStringPredicatesAndRejectsInvalidRegularExpressions) {
	const auto result = execute(
			"RETURN 'graph' STARTS WITH 'gr' AS starts, 'graph' ENDS WITH 'ph' AS ends, "
			"'graph' CONTAINS 'rap' AS contains_value, 'graph42' =~ 'graph[0-9]+' AS regex_match, "
			"'graph' =~ '[0-9]+' AS regex_miss, null STARTS WITH 'g' AS null_starts, "
			"'graph' ENDS WITH null AS null_ends, null CONTAINS null AS null_contains");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("starts").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("ends").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("contains_value").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("regex_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("regex_miss").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("null_starts").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("null_ends").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("null_contains").asPrimitive().getType(), PropertyType::NULL_TYPE);

	EXPECT_THROW(static_cast<void>(execute("RETURN 'graph' =~ '[invalid' AS invalid_regex")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesTypedComparisonOperatorsThroughThePublicQueryEngine) {
	const auto result = execute(
			"UNWIND [{lower: 1, equal: 2, higher: 3, text: 'graph'}] AS value "
			"RETURN value.lower < value.equal AS less, value.equal <= value.equal AS less_equal, "
			"value.higher > value.equal AS greater, value.equal >= value.equal AS greater_equal, "
			"value.equal = 2 AS equals, value.equal <> value.higher AS not_equals, "
			"value.text = 'graph' AS string_equals, value.text <> 'database' AS string_not_equals, "
			"null = 1 AS null_equals");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("less").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("less_equal").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("greater").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("greater_equal").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("equals").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("not_equals").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("string_equals").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("string_not_equals").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("null_equals").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesPublicArithmeticTypesAndRejectsInvalidOperands) {
	const auto result = execute(
			"RETURN 7 + 2 AS integer_sum, 7.5 / 2.5 AS decimal_division, 17 % 5 AS remainder, "
			"2 ^ 10 AS power, 'node-' + 7 AS concatenated, null + 1 AS null_sum");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("integer_sum").asPrimitive(), PropertyValue(9));
	EXPECT_EQ(row.at("decimal_division").asPrimitive(), PropertyValue(3.0));
	EXPECT_EQ(row.at("remainder").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("power").asPrimitive(), PropertyValue(1024.0));
	EXPECT_EQ(row.at("concatenated").asPrimitive(), PropertyValue("node-7"));
	EXPECT_EQ(row.at("null_sum").asPrimitive().getType(), PropertyType::NULL_TYPE);

	EXPECT_THROW(static_cast<void>(execute("RETURN 1 / 0 AS invalid_division")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN 1 % 0 AS invalid_modulo")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN true + 1 AS invalid_numeric_type")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesPublicUnaryTypesAndRejectsLegacyTruthiness) {
	const auto result = execute(
			"UNWIND [{integer: 42, decimal: 3.5, enabled: true, disabled: false}] AS value "
			"RETURN -value.integer AS negative_integer, -value.decimal AS negative_decimal, "
			"-null AS null_negation, NOT value.enabled AS not_true, NOT value.disabled AS not_false, "
			"NOT null AS null_not");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("negative_integer").asPrimitive(), PropertyValue(-42));
	EXPECT_EQ(row.at("negative_decimal").asPrimitive(), PropertyValue(-3.5));
	EXPECT_EQ(row.at("null_negation").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("not_true").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("not_false").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("null_not").asPrimitive().getType(), PropertyType::NULL_TYPE);

	EXPECT_THROW(static_cast<void>(execute("RETURN -true AS invalid_negation")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN NOT 1 AS invalid_numeric_truthiness")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN NOT 'truthy' AS invalid_string_truthiness")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesDynamicArithmeticPrecedenceAndRejectsLegacyCoercions) {
	const auto result = execute(
			"UNWIND [{integer: 7, decimal: 2.5, divisor: 2, values: [9]}] AS value "
			"RETURN value.integer + value.decimal AS mixed_addition, "
			"value.integer / value.divisor AS fractional_division, "
			"value.integer + value.decimal * value.divisor AS precedence, "
			"(-value.integer + value.decimal) AS unary_grouping, "
			"(value.integer - 1) ^ value.divisor AS power, "
			"+value.values[0] AS unary_plus_accessor");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("mixed_addition").asPrimitive(), PropertyValue(9.5));
	EXPECT_EQ(row.at("fractional_division").asPrimitive(), PropertyValue(3.5));
	EXPECT_EQ(row.at("precedence").asPrimitive(), PropertyValue(12.0));
	EXPECT_EQ(row.at("unary_grouping").asPrimitive(), PropertyValue(-4.5));
	EXPECT_EQ(row.at("power").asPrimitive(), PropertyValue(36.0));
	EXPECT_EQ(row.at("unary_plus_accessor").asPrimitive(), PropertyValue(9));

	EXPECT_THROW(static_cast<void>(execute("RETURN 7.5 % 2 AS invalid_fractional_modulo")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN true - 1 AS invalid_boolean_numeric_coercion")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesNestedArithmeticWithVariablesAndStableNumericResults) {
	const auto result = execute(
			"UNWIND [{a: 10, b: 5, c: 2, d: 3.5, e: 2.5}] AS value "
			"RETURN (value.a + value.b) * value.c AS left_grouped, "
			"value.a * (value.b + value.c) AS right_grouped, "
			"((value.a + value.b) * (value.c - value.a)) / value.d AS nested, "
			"value.a - value.b / value.c AS division_precedence, "
			"value.a * value.b ^ value.c AS power_precedence, "
			"1000000 * 1000000 + 500000 AS large_integer, "
			"0.1 * 0.2 + 0.3 AS fractional");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("left_grouped").asPrimitive(), PropertyValue(30));
	EXPECT_EQ(row.at("right_grouped").asPrimitive(), PropertyValue(70));
	EXPECT_EQ(row.at("nested").asPrimitive(), PropertyValue(-34.285714285714285));
	EXPECT_EQ(row.at("division_precedence").asPrimitive(), PropertyValue(7.5));
	EXPECT_EQ(row.at("power_precedence").asPrimitive(), PropertyValue(250.0));
	EXPECT_EQ(row.at("large_integer").asPrimitive(), PropertyValue(int64_t(1000000500000LL)));
	EXPECT_DOUBLE_EQ(std::get<double>(row.at("fractional").asPrimitive().getVariant()), 0.32);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesListIndexAndSliceBoundariesThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN [1, 2, 3][-1] AS last, [1, 2, 3][99] AS missing, [1, 2, 3][..2] AS prefix, "
			"[1, 2, 3][1..] AS suffix, [1, 2, 3][-2..-1] AS negative_slice");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("last").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("missing").asPrimitive().getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(row.at("prefix").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
	EXPECT_EQ(row.at("suffix").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
	EXPECT_EQ(row.at("negative_slice").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue(2)}));

	EXPECT_THROW(static_cast<void>(execute("RETURN [1, 2][true] AS invalid_index")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN [1, 2][1.0] AS fractional_index")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN [1, 2][0..1.0] AS fractional_bound")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN 1[0] AS invalid_source")), std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesReduceOverAListLiteral) {
	const auto result = execute(
			"RETURN reduce(total = 0, value IN [1, 2, 3] | total + value) AS total, "
			"reduce(text = '', value IN ['a', 'b', 'c'] | text + value) AS joined, "
			"reduce(seed = 42, value IN [] | seed + value) AS empty");

	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(6));
	EXPECT_EQ(result.getRows().front().at("joined").asPrimitive(), PropertyValue("abc"));
	EXPECT_EQ(result.getRows().front().at("empty").asPrimitive(), PropertyValue(42));
	EXPECT_THROW(static_cast<void>(execute("RETURN reduce(total = 0, value IN 1 | total + value) AS invalid")),
			 std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, RejectsInvalidStructuralExpressionInputs) {
	EXPECT_THROW(static_cast<void>(execute("RETURN [1, 2]['invalid'] AS value")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN [value IN 1 | value] AS value")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN all(value IN 1 WHERE value > 0) AS value")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN reduce(total = 0, value IN 1 | total + value) AS value")),
			 std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, RejectsNonBooleanPredicateContexts) {
	EXPECT_THROW(static_cast<void>(execute("RETURN CASE WHEN 1 THEN 'invalid' ELSE 'fallback' END AS value")),
			 std::runtime_error);

	(void) execute("CREATE (:Person)-[:KNOWS]->(:Person)");
	EXPECT_THROW(static_cast<void>(execute(
				"MATCH (n:Person) RETURN EXISTS((n)-[:KNOWS]->(m) WHERE 1) AS invalid_predicate")),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute(
				"MATCH (n:Person) RETURN [(n)-[:KNOWS]->(m) WHERE 1 | m] AS invalid_predicate")),
			 std::runtime_error);
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesExistsAndPatternComprehensionAgainstNativeGraphData) {
	(void) execute("CREATE (:Person {name: 'Ada'})-[:KNOWS]->(:Friend {name: 'Bob', age: 30})");
	(void) execute("CREATE (:Person {name: 'Grace'})-[:KNOWS]->(:Friend {name: 'Lin', age: 20})");

	const auto result = execute(
			"MATCH (n:Person) "
			"RETURN n.name AS name, "
			"EXISTS((n)-[:KNOWS]->(m:Friend) WHERE m.age > 21) AS has_adult_friend, "
			"[(n)-[:KNOWS]->(m:Friend) WHERE m.age > 21 | m.name] AS adult_friends "
			"ORDER BY name ASC");

	ASSERT_EQ(result.rowCount(), 2U);
	const auto &ada = result.getRows()[0];
	EXPECT_EQ(ada.at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(ada.at("has_adult_friend").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(ada.at("adult_friends").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("Bob")}));
	const auto &grace = result.getRows()[1];
	EXPECT_EQ(grace.at("name").asPrimitive(), PropertyValue("Grace"));
	EXPECT_EQ(grace.at("has_adult_friend").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(grace.at("adult_friends").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST_F(CypherStructuralExpressionsTest, PreservesIncomingCreateDirectionThroughNativeStorage) {
	(void) execute("CREATE (:IncomingPathStart {name: 'start'})<-[:BACK]-(:IncomingPathEnd {name: 'end'})");

	const auto incoming = execute(
			"MATCH (start:IncomingPathStart)<-[:BACK]-(end:IncomingPathEnd) RETURN start.name AS start, end.name AS end");
	ASSERT_EQ(incoming.rowCount(), 1U);
	EXPECT_EQ(incoming.getRows().front().at("start").asPrimitive(), PropertyValue("start"));
	EXPECT_EQ(incoming.getRows().front().at("end").asPrimitive(), PropertyValue("end"));

	const auto outgoing = execute(
			"MATCH (start:IncomingPathStart)-[:BACK]->(end:IncomingPathEnd) RETURN start");
	EXPECT_EQ(outgoing.rowCount(), 0U);
}

TEST_F(CypherStructuralExpressionsTest, PreservesPatternExpressionDirectionsAndTypeLabelFilters) {
	(void) execute("CREATE (:PatternFocus {name: 'focus'})-[:OUT]->(:PatternPerson {name: 'person'})");
	(void) execute("MATCH (focus:PatternFocus) CREATE (focus)-[:OUT]->(:PatternCompany {name: 'company'})");
	(void) execute("MATCH (focus:PatternFocus) CREATE (:PatternSource {name: 'source'})-[:IN]->(focus)");

	const auto result = execute(
			"MATCH (focus:PatternFocus) "
			"RETURN EXISTS((focus)-[:OUT]->(:PatternCompany)) AS typed_label_match, "
			"EXISTS((focus)-[:OUT]->(:PatternMissing)) AS typed_label_miss, "
			"EXISTS((focus)<-[:IN]-()) AS incoming_match, EXISTS((focus)-[]->()) AS any_outgoing, "
			"[(focus)-[:OUT]->(target:PatternPerson) | target.name] AS typed_outgoing, "
			"[(focus)<-[:IN]-(source:PatternSource) | source.name] AS typed_incoming, "
			"[(focus)-[:OUT]-(neighbor:PatternCompany) | neighbor.name] AS undirected");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("typed_label_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("typed_label_miss").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("incoming_match").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("any_outgoing").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("typed_outgoing").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("person")}));
	EXPECT_EQ(row.at("typed_incoming").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("source")}));
	EXPECT_EQ(row.at("undirected").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("company")}));
}

TEST_F(CypherStructuralExpressionsTest, EvaluatesEntityIntrospectionAgainstNativeGraphData) {
	(void) execute("CREATE (a:Person:Employee {name: 'Ada', age: 30})-[:KNOWS {since: 2024}]->(b:Person)");

	const auto result = execute(
			"MATCH (a:Employee)-[r:KNOWS]->(b) "
			"RETURN id(a) AS node_id, labels(a) AS node_labels, type(r) AS edge_type, "
			"keys(a) AS node_keys, properties(r) AS edge_properties");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("node_id").asPrimitive().getType(), PropertyType::INTEGER);
	EXPECT_EQ(row.at("node_labels").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("Employee"), PropertyValue("Person")}));
	EXPECT_EQ(row.at("edge_type").asPrimitive(), PropertyValue("KNOWS"));
	EXPECT_EQ(row.at("node_keys").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("age"), PropertyValue("name")}));
	EXPECT_EQ(row.at("edge_properties").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"since", PropertyValue(2024)}}));
}

TEST_F(CypherStructuralExpressionsTest, MaterializesNativePathFunctionsWithOrderedEntityValues) {
	(void) execute("CREATE (:Person {name: 'Ada'})-[:KNOWS {since: 2024}]->(:Person {name: 'Bob'})");

	const auto result = execute(
			"MATCH p = (a:Person {name: 'Ada'})-[r:KNOWS]->(b:Person {name: 'Bob'}) "
			"RETURN length(p) AS path_length, nodes(p) AS path_nodes, relationships(p) AS path_relationships");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("path_length").asPrimitive(), PropertyValue(1));

	const auto &nodes = row.at("path_nodes").asPrimitive().getList();
	ASSERT_EQ(nodes.size(), 2U);
	EXPECT_EQ(nodes[0].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(nodes[0].getMap().at("name"), PropertyValue("Ada"));
	EXPECT_EQ(nodes[1].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(nodes[1].getMap().at("name"), PropertyValue("Bob"));

	const auto &relationships = row.at("path_relationships").asPrimitive().getList();
	ASSERT_EQ(relationships.size(), 1U);
	EXPECT_EQ(relationships[0].getMap().at("_type"), PropertyValue("relationship"));
	EXPECT_EQ(relationships[0].getMap().at("since"), PropertyValue(2024));
}

TEST_F(CypherStructuralExpressionsTest, MaterializesNativeMapProjectionsForEntitiesAndMaps) {
	(void) execute("CREATE (:Person {name: 'Ada', age: 30})-[:KNOWS {since: 2024}]->(:Person {name: 'Bob'})");

	const auto entityResult = execute(
			"MATCH (a:Person {name: 'Ada'})-[r:KNOWS]->() "
			"RETURN a {.name, years: a.age, .*} AS person, r {.since, category: type(r), .*} AS relationship");

	ASSERT_EQ(entityResult.rowCount(), 1U);
	const auto &entityRow = entityResult.getRows().front();
	EXPECT_EQ(entityRow.at("person").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"age", PropertyValue(30)},
															{"name", PropertyValue("Ada")},
															{"years", PropertyValue(30)}}));
	EXPECT_EQ(entityRow.at("relationship").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"category", PropertyValue("KNOWS")},
															{"since", PropertyValue(2024)}}));

	const auto mapResult = execute(
			"MATCH (n:Person {name: 'Ada'}) WITH properties(n) AS profile "
			"RETURN profile {.name, years: profile.age, .*} AS projection");

	ASSERT_EQ(mapResult.rowCount(), 1U);
	EXPECT_EQ(mapResult.getRows().front().at("projection").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"age", PropertyValue(30)},
															{"name", PropertyValue("Ada")},
															{"years", PropertyValue(30)}}));

	const auto nullLiteralResult = execute(
			"MATCH (n:Person {name: 'Ada'}) RETURN n {name: n.name, absent: null} AS projection");
	ASSERT_EQ(nullLiteralResult.rowCount(), 1U);
	const auto &nullLiteral = nullLiteralResult.getRows().front().at("projection").asPrimitive().getMap();
	EXPECT_EQ(nullLiteral.at("name"), PropertyValue("Ada"));
	EXPECT_EQ(nullLiteral.at("absent"), PropertyValue());
}

TEST_F(CypherStructuralExpressionsTest, PreservesNullForMissingMapProjectionProperties) {
	(void) execute("CREATE (:Person {name: 'Ada'})");

	const auto result = execute("MATCH (n:Person) RETURN n {.name, .missing} AS projection");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &projection = result.getRows().front().at("projection").asPrimitive().getMap();
	EXPECT_EQ(projection.at("name"), PropertyValue("Ada"));
	EXPECT_EQ(projection.at("missing").getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, RejectsScalarMapProjectionBindingsAndPropagatesNullBindings) {
	(void) execute("CREATE (:Person {name: 'Ada', age: 30})");

	EXPECT_THROW(
			static_cast<void>(execute(
					"MATCH (n:Person) WITH n.age AS scalar RETURN scalar {.value} AS projection")),
			std::runtime_error);

	const auto nullResult = execute(
			"MATCH (n:Person) OPTIONAL MATCH (n)-[:KNOWS]->(m) RETURN m {.name, .*} AS projection");
	ASSERT_EQ(nullResult.rowCount(), 1U);
	EXPECT_EQ(nullResult.getRows().front().at("projection").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST_F(CypherStructuralExpressionsTest, MaterializesNestedMapAndListLiteralsThroughTheNativeQueryEngine) {
	const auto result = execute(
			"RETURN {name: 'Ada', tags: ['graph', 'database'], details: {score: 7, active: true}} AS value");

	ASSERT_EQ(result.rowCount(), 1U);
	PropertyValue::MapType details;
	details.emplace("score", PropertyValue(7));
	details.emplace("active", PropertyValue(true));
	PropertyValue::MapType expected;
	expected.emplace("name", PropertyValue("Ada"));
	expected.emplace("tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("graph"), PropertyValue("database")}));
	expected.emplace("details", PropertyValue(std::move(details)));
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(expected));
}

TEST_F(CypherStructuralExpressionsTest, MaterializesShortestPathThroughTheNativeQueryEngine) {
	(void) execute("CREATE (:PathStart {name: 'start'})-[:PATH {rank: 1}]->(:PathMiddle {name: 'middle'})");
	(void) execute(
			"MATCH (middle:PathMiddle {name: 'middle'}) "
			"CREATE (middle)-[:PATH {rank: 2}]->(:PathEnd {name: 'end'})");

	const auto result = execute(
			"MATCH (start:PathStart), (end:PathEnd) "
			"RETURN shortestPath((start)-[:PATH*1..2]->(end)) AS path");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 5U);
	EXPECT_EQ(path[0].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(path[0].getMap().at("name"), PropertyValue("start"));
	EXPECT_EQ(path[1].getMap().at("_type"), PropertyValue("relationship"));
	EXPECT_EQ(path[1].getMap().at("rank"), PropertyValue(1));
	EXPECT_EQ(path[2].getMap().at("name"), PropertyValue("middle"));
	EXPECT_EQ(path[3].getMap().at("rank"), PropertyValue(2));
	EXPECT_EQ(path[4].getMap().at("name"), PropertyValue("end"));
}

TEST_F(CypherStructuralExpressionsTest, MaterializesAllShortestPathsThroughTheNativeQueryEngine) {
	(void) execute("CREATE (:AllPathStart {name: 'start'})-[:PATH]->(:AllPathMiddle {name: 'left'})");
	(void) execute(
			"MATCH (start:AllPathStart {name: 'start'}) "
			"CREATE (start)-[:PATH]->(:AllPathMiddle {name: 'right'})");
	(void) execute(
			"MATCH (middle:AllPathMiddle {name: 'left'}) "
			"CREATE (middle)-[:PATH]->(:AllPathEnd {name: 'end'})");
	(void) execute(
			"MATCH (middle:AllPathMiddle {name: 'right'}) "
			"MATCH (end:AllPathEnd {name: 'end'}) CREATE (middle)-[:PATH]->(end)");

	const auto result = execute(
			"MATCH (start:AllPathStart), (end:AllPathEnd) "
			"RETURN allShortestPaths((start)-[:PATH*2]->(end)) AS paths");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &paths = result.getRows().front().at("paths").asPrimitive().getList();
	ASSERT_EQ(paths.size(), 2U);
	ASSERT_EQ(paths[0].getList().size(), 5U);
	ASSERT_EQ(paths[1].getList().size(), 5U);
	EXPECT_EQ(paths[0].getList()[2].getMap().at("name"), PropertyValue("left"));
	EXPECT_EQ(paths[1].getList()[2].getMap().at("name"), PropertyValue("right"));
}

TEST_F(CypherStructuralExpressionsTest, PreservesNullForUnavailableOrOutOfRangeShortestPath) {
	(void) execute("CREATE (:BoundedPathStart {name: 'start'})-[:PATH]->(:BoundedPathMiddle)");
	(void) execute(
			"MATCH (middle:BoundedPathMiddle) CREATE (middle)-[:PATH]->(:BoundedPathEnd {name: 'end'})");
	(void) execute("CREATE (:BoundedPathEnd {name: 'disconnected'})");

	const auto result = execute(
			"MATCH (start:BoundedPathStart), (end:BoundedPathEnd) "
			"RETURN end.name AS name, shortestPath((start)-[:PATH*1]->(end)) AS path ORDER BY name ASC");

	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row: result.getRows()) {
		EXPECT_EQ(row.at("path").asPrimitive().getType(), PropertyType::NULL_TYPE);
	}
}
