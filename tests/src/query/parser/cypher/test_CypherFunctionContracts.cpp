/**
 * @file test_CypherFunctionContracts.cpp
 * @brief End-to-end public contracts for representative scalar-function families.
 */

#include "QueryTestFixture.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <variant>

using namespace graph;

class CypherFunctionContractsTest : public QueryTestFixture {};

TEST_F(CypherFunctionContractsTest, ExecutesStringFunctionsThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN upper('Ada') AS upper_name, lower('GRAPH') AS lower_name, "
			"substring('graph', 1, 3) AS middle, replace('a-b-a', 'a', 'x') AS replaced, "
			"split('a,b,c', ',') AS pieces, reverse('abc') AS reversed");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("upper_name").asPrimitive(), PropertyValue("ADA"));
	EXPECT_EQ(row.at("lower_name").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("middle").asPrimitive(), PropertyValue("rap"));
	EXPECT_EQ(row.at("replaced").asPrimitive(), PropertyValue("x-b-x"));
	EXPECT_EQ(row.at("pieces").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b"), PropertyValue("c")}));
	EXPECT_EQ(row.at("reversed").asPrimitive(), PropertyValue("cba"));
}

TEST_F(CypherFunctionContractsTest, ExecutesRemainingStringFunctionsAndAliasesThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN toString(42) AS rendered, trim('  graph  ') AS trimmed, length('graph') AS length_value, "
			"startsWith('graph', 'gr') AS starts, endsWith('graph', 'ph') AS ends, "
			"contains('graph', 'rap') AS contains_value, left('graph', 2) AS left_value, "
			"right('graph', 2) AS right_value, lTrim('  graph') AS left_trimmed, "
			"rTrim('graph  ') AS right_trimmed, toUpper('graph') AS upper_alias, "
			"toLower('GRAPH') AS lower_alias, reverse([1, 2, 3]) AS reversed_list");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("rendered").asPrimitive(), PropertyValue("42"));
	EXPECT_EQ(row.at("trimmed").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("length_value").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("starts").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("ends").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("contains_value").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("left_value").asPrimitive(), PropertyValue("gr"));
	EXPECT_EQ(row.at("right_value").asPrimitive(), PropertyValue("ph"));
	EXPECT_EQ(row.at("left_trimmed").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("right_trimmed").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("upper_alias").asPrimitive(), PropertyValue("GRAPH"));
	EXPECT_EQ(row.at("lower_alias").asPrimitive(), PropertyValue("graph"));
	EXPECT_EQ(row.at("reversed_list").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(1)}));

	EXPECT_THROW(static_cast<void>(execute("RETURN left('graph') AS invalid")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, ExecutesMathListAndConversionFunctionsThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN abs(-4) AS absolute, pow(2, 3) AS power, range(1, 5) AS generated, "
			"head([7, 8]) AS first, tail([7, 8]) AS rest, last([7, 8]) AS last_value, "
			"coalesce(null, 'fallback') AS fallback, toInteger('42') AS integer_value, "
			"toFloat('3.5') AS float_value, toBoolean('true') AS boolean_value");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("absolute").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("power").asPrimitive(), PropertyValue(8.0));
	EXPECT_EQ(row.at("generated").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3), PropertyValue(4),
																		PropertyValue(5)}));
	EXPECT_EQ(row.at("first").asPrimitive(), PropertyValue(7));
	EXPECT_EQ(row.at("rest").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue(8)}));
	EXPECT_EQ(row.at("last_value").asPrimitive(), PropertyValue(8));
	EXPECT_EQ(row.at("fallback").asPrimitive(), PropertyValue("fallback"));
	EXPECT_EQ(row.at("integer_value").asPrimitive(), PropertyValue(42));
	EXPECT_EQ(row.at("float_value").asPrimitive(), PropertyValue(3.5));
	EXPECT_EQ(row.at("boolean_value").asPrimitive(), PropertyValue(true));
}

TEST_F(CypherFunctionContractsTest, ExecutesRemainingMathFunctionsAndDomainBoundariesThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN ceil(1.2) AS ceiling, floor(1.8) AS floor_value, round(1.6) AS rounded, sqrt(9) AS root, "
			"sign(-4) AS sign_value, log(1) AS natural_log, log10(100) AS base_ten_log, exp(0) AS exponent, "
			"sin(0) AS sine, cos(0) AS cosine, tan(0) AS tangent, asin(0) AS arcsine, acos(0) AS arccosine, "
			"atan(1) AS arctangent, atan2(1, 0) AS arctangent_two, pi() AS pi_value, e() AS e_value, rand() AS random_value, "
			"sqrt(-1) AS invalid_root, asin(2) AS invalid_arcsine");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	const auto number = [&row](const std::string &name) {
		return std::get<double>(row.at(name).asPrimitive().getVariant());
	};
	EXPECT_DOUBLE_EQ(number("ceiling"), 2.0);
	EXPECT_DOUBLE_EQ(number("floor_value"), 1.0);
	EXPECT_DOUBLE_EQ(number("rounded"), 2.0);
	EXPECT_DOUBLE_EQ(number("root"), 3.0);
	EXPECT_EQ(row.at("sign_value").asPrimitive(), PropertyValue(-1));
	EXPECT_DOUBLE_EQ(number("natural_log"), 0.0);
	EXPECT_DOUBLE_EQ(number("base_ten_log"), 2.0);
	EXPECT_DOUBLE_EQ(number("exponent"), 1.0);
	EXPECT_DOUBLE_EQ(number("sine"), 0.0);
	EXPECT_DOUBLE_EQ(number("cosine"), 1.0);
	EXPECT_DOUBLE_EQ(number("tangent"), 0.0);
	EXPECT_DOUBLE_EQ(number("arcsine"), 0.0);
	EXPECT_NEAR(number("arccosine"), std::numbers::pi / 2.0, 1e-12);
	EXPECT_NEAR(number("arctangent"), std::numbers::pi / 4.0, 1e-12);
	EXPECT_NEAR(number("arctangent_two"), std::numbers::pi / 2.0, 1e-12);
	EXPECT_NEAR(number("pi_value"), std::numbers::pi, 1e-15);
	EXPECT_NEAR(number("e_value"), std::numbers::e, 1e-15);
	EXPECT_GE(number("random_value"), 0.0);
	EXPECT_LE(number("random_value"), 1.0);
	EXPECT_EQ(row.at("invalid_root").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("invalid_arcsine").asPrimitive(), PropertyValue());
}

TEST_F(CypherFunctionContractsTest, ExecutesListBoundariesAndRejectsAnInvalidRangeStepThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN size([1, 2, 3]) AS list_size, size('graph') AS string_size, range(5, 1, -2) AS descending, "
			"head([]) AS empty_head, tail([]) AS empty_tail, last([]) AS empty_last, coalesce(null, null, 7) AS fallback");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("list_size").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("string_size").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("descending").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(5), PropertyValue(3), PropertyValue(1)}));
	EXPECT_EQ(row.at("empty_head").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("empty_tail").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_EQ(row.at("empty_last").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("fallback").asPrimitive(), PropertyValue(7));
	EXPECT_THROW(static_cast<void>(execute("RETURN range(1, 3, 0) AS invalid")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, ExecutesTemporalAndBytesFunctionsAndRejectsInvalidArity) {
	const auto result = execute(
			"RETURN date('2024-01-30') AS date_value, duration('P2D') AS duration_value, "
			"bytes('0x00ff') AS byte_value");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("date_value").asPrimitive().getVariant()).toISO(), "2024-01-30");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("duration_value").asPrimitive().getVariant()),
			  (TemporalDuration{0, 2, 0}));
	EXPECT_EQ(row.at("byte_value").asPrimitive().getBytes(),
			  (PropertyValue::Bytes{std::byte{0x00}, std::byte{0xFF}}));
	EXPECT_THROW(static_cast<void>(execute("RETURN upper(1, 2) AS invalid")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, ExecutesTemporalMapAndUtilityFunctionsThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN date({year: 2024, month: 2, day: 3}) AS date_map, "
			"datetime('2024-01-15T13:45:30') AS datetime_value, "
			"duration({days: 2, hours: 3}) AS duration_map, timestamp() AS timestamp_value, "
			"randomUUID() AS uuid_value, bytes([0, 128, 255]) AS byte_list");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("date_map").asPrimitive().getVariant()).toISO(), "2024-02-03");
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("datetime_value").asPrimitive().getVariant()).toISO(),
			  "2024-01-15T13:45:30");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("duration_map").asPrimitive().getVariant()).toISO(), "P2DT3H");
	EXPECT_GT(std::get<int64_t>(row.at("timestamp_value").asPrimitive().getVariant()), 0);
	const std::string &uuid = std::get<std::string>(row.at("uuid_value").asPrimitive().getVariant());
	ASSERT_EQ(uuid.size(), 36U);
	EXPECT_EQ(uuid[14], '4');
	EXPECT_EQ(uuid[8], '-');
	EXPECT_EQ(uuid[13], '-');
	EXPECT_EQ(uuid[18], '-');
	EXPECT_EQ(uuid[23], '-');
	EXPECT_EQ(row.at("byte_list").asPrimitive().getBytes(),
			  (PropertyValue::Bytes{std::byte{0x00}, std::byte{0x80}, std::byte{0xFF}}));
	EXPECT_THROW(static_cast<void>(execute("RETURN timestamp(1) AS invalid")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, ExecutesTemporalDefaultsAndCopiesBytesThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN date() AS current_date, datetime() AS current_datetime, "
			"bytes(bytes('0X0aFF')) AS copied_bytes, bytes('ff') AS unprefixed_bytes, "
			"bytes('') AS empty_bytes");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_TRUE(std::holds_alternative<TemporalDate>(row.at("current_date").asPrimitive().getVariant()));
	EXPECT_TRUE(std::holds_alternative<TemporalDateTime>(row.at("current_datetime").asPrimitive().getVariant()));
	EXPECT_EQ(row.at("copied_bytes").asPrimitive().getBytes(),
			  (PropertyValue::Bytes{std::byte{0x0A}, std::byte{0xFF}}));
	EXPECT_EQ(row.at("unprefixed_bytes").asPrimitive().getBytes(), (PropertyValue::Bytes{std::byte{0xFF}}));
	EXPECT_EQ(row.at("empty_bytes").asPrimitive().getBytes(), (PropertyValue::Bytes{}));
}

TEST_F(CypherFunctionContractsTest, AppliesTemporalMapDefaultsThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN date({}) AS default_date, datetime({}) AS default_datetime, duration({}) AS default_duration");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(std::get<TemporalDate>(row.at("default_date").asPrimitive().getVariant()).toISO(), "1970-01-01");
	EXPECT_EQ(std::get<TemporalDateTime>(row.at("default_datetime").asPrimitive().getVariant()).toISO(),
			  "1970-01-01T00:00:00");
	EXPECT_EQ(std::get<TemporalDuration>(row.at("default_duration").asPrimitive().getVariant()).toISO(), "P0D");
}

TEST_F(CypherFunctionContractsTest, PreservesFunctionNullAndEmptyPathCollectionSemantics) {
	const auto result = execute(
			"RETURN toString(null) AS string_null, toInteger(null) AS integer_null, toFloat(null) AS float_null, "
			"toBoolean(null) AS boolean_null, "
			"date(null) AS date_null, datetime(null) AS datetime_null, duration(null) AS duration_null, "
			"bytes(null) AS bytes_null, nodes([]) AS empty_nodes, relationships([]) AS empty_relationships");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("string_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("integer_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("float_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("boolean_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("date_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("datetime_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("duration_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("bytes_null").asPrimitive(), PropertyValue());
	EXPECT_EQ(row.at("empty_nodes").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_EQ(row.at("empty_relationships").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST_F(CypherFunctionContractsTest, RejectsMalformedFunctionValuesWithoutPartialConversion) {
	EXPECT_THROW(static_cast<void>(execute("RETURN toInteger('42x') AS invalid_integer")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes('0x0') AS invalid_bytes")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes('0x0g') AS invalid_bytes")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes([0, 'bad']) AS invalid_bytes")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes([-1]) AS invalid_bytes")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes([0, 256]) AS invalid_bytes")), std::invalid_argument);
}

TEST_F(CypherFunctionContractsTest, RejectsInvalidTemporalAndByteArgumentTypesThroughThePublicQueryEngine) {
	EXPECT_THROW(static_cast<void>(execute("RETURN date(1) AS invalid_date")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN datetime(true) AS invalid_datetime")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN duration(date('2024-01-01')) AS invalid_duration")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN bytes(true) AS invalid_bytes")), std::invalid_argument);
}

TEST_F(CypherFunctionContractsTest, RejectsLegacyScalarFunctionCoercionsThroughThePublicQueryEngine) {
	EXPECT_THROW(static_cast<void>(execute("RETURN upper(42) AS invalid_upper")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN substring('graph', 1.5) AS invalid_substring")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN startsWith('graph', 1) AS invalid_prefix")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN replace('graph', 'g', 1) AS invalid_replace")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN left('graph', true) AS invalid_left")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN reverse(42) AS invalid_reverse")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN head('graph') AS invalid_head")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN size(42) AS invalid_size")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN sqrt('9') AS invalid_sqrt")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN pow(true, 2) AS invalid_power")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN atan2(1, '0') AS invalid_atan2")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, RejectsNonFiniteAndOutOfRangeConversionsThroughThePublicQueryEngine) {
	graph::query::QueryContext nonFinite;
	nonFinite.parameters.emplace("value", PropertyValue(std::numeric_limits<double>::quiet_NaN()));
	for (const std::string &query: {"RETURN toInteger($value) AS converted", "RETURN toFloat($value) AS converted",
									 "RETURN toBoolean($value) AS converted"}) {
		EXPECT_THROW(static_cast<void>(db->getQueryEngine()->execute(query, nonFinite)), std::runtime_error) << query;
	}

	graph::query::QueryContext outOfRange;
	outOfRange.parameters.emplace("value", PropertyValue(std::numeric_limits<double>::max()));
	EXPECT_THROW(static_cast<void>(db->getQueryEngine()->execute("RETURN toInteger($value) AS converted", outOfRange)),
			 std::runtime_error);

	graph::query::QueryContext overflowText;
	overflowText.parameters.emplace("value", PropertyValue("1e309"));
	EXPECT_THROW(static_cast<void>(db->getQueryEngine()->execute("RETURN toFloat($value) AS converted", overflowText)),
			 std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, AppliesOnlyExplicitNativeToBooleanConversionsThroughThePublicQueryEngine) {
	const auto result = execute(
			"RETURN toBoolean(true) AS boolean_value, toBoolean(0) AS zero, toBoolean(-2.5) AS number, "
			"toBoolean('TRUE') AS true_text, toBoolean('false') AS false_text");
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("boolean_value").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("zero").asPrimitive(), PropertyValue(false));
	EXPECT_EQ(row.at("number").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("true_text").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(row.at("false_text").asPrimitive(), PropertyValue(false));

	EXPECT_THROW(static_cast<void>(execute("RETURN toBoolean('yes') AS invalid")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN toBoolean([]) AS invalid")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, RejectsMalformedTemporalValuesInsteadOfNormalizingThem) {
	EXPECT_THROW(static_cast<void>(execute("RETURN date('2024-02-30') AS invalid_date")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN date('2024-01-02ignored') AS invalid_date")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN datetime('2024-13-02T12:00:00') AS invalid_datetime")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN datetime('2024-01-02T12:00:00ignored') AS invalid_datetime")),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("RETURN duration('P1') AS invalid_duration")), std::runtime_error);
}

TEST_F(CypherFunctionContractsTest, RejectsMalformedTemporalMapComponentsInsteadOfDefaultingThem) {
	EXPECT_THROW(static_cast<void>(execute("RETURN date({year: 2024, month: 'February', day: 3}) AS invalid_date")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute(
					"RETURN datetime({year: 2024, month: 2, day: 3, hour: 12, minute: true}) AS invalid_datetime")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN duration({days: 'two'}) AS invalid_duration")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("RETURN date({year: 999999999999, month: 1, day: 1}) AS invalid_date")),
			 std::out_of_range);
	EXPECT_THROW(static_cast<void>(execute("RETURN date({year: -999999999999, month: 1, day: 1}) AS invalid_date")),
			 std::out_of_range);
}
