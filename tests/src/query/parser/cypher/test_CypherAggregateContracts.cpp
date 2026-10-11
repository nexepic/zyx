/**
 * @file test_CypherAggregateContracts.cpp
 * @brief Public QueryEngine contracts for Cypher aggregate behavior.
 */

#include "QueryTestFixture.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace graph;

class CypherAggregateContractsTest : public QueryTestFixture {};

TEST_F(CypherAggregateContractsTest, ExecutesGlobalAggregatesWithDistinctAndNullSemantics) {
	const auto result = execute(
			"UNWIND [1, 1, 2, null] AS value "
			"RETURN count(*) AS row_count, count(value) AS value_count, count(DISTINCT value) AS distinct_count, "
			"sum(value) AS total, avg(value) AS average, min(value) AS minimum, max(value) AS maximum, "
			"collect(DISTINCT value) AS values");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("row_count").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("value_count").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(row.at("distinct_count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("total").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("average").asPrimitive(), PropertyValue(4.0 / 3.0));
	EXPECT_EQ(row.at("minimum").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("maximum").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("values").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
}

TEST_F(CypherAggregateContractsTest, GroupsAndOrdersAggregateRows) {
	const auto result = execute(
			"UNWIND [2, 1, 2, 1, 1] AS value "
			"RETURN value, count(*) AS amount, sum(value) AS total ORDER BY value ASC");

	ASSERT_EQ(result.rowCount(), 2U);
	const auto &first = result.getRows()[0];
	EXPECT_EQ(first.at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(first.at("amount").asPrimitive(), PropertyValue(3));
	EXPECT_EQ(first.at("total").asPrimitive(), PropertyValue(3));
	const auto &second = result.getRows()[1];
	EXPECT_EQ(second.at("value").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(second.at("amount").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(second.at("total").asPrimitive(), PropertyValue(4));
}

TEST_F(CypherAggregateContractsTest, ExecutesStatisticalAndPercentileAggregates) {
	const auto result = execute(
			"UNWIND [1, 2, 3, 4] AS value "
			"RETURN stdev(value) AS sample_stdev, stdevp(value) AS population_stdev, "
			"percentileDisc(value, 0.5) AS discrete_median, percentileCont(value, 0.5) AS continuous_median");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_DOUBLE_EQ(std::get<double>(row.at("sample_stdev").asPrimitive().getVariant()), std::sqrt(5.0 / 3.0));
	EXPECT_DOUBLE_EQ(std::get<double>(row.at("population_stdev").asPrimitive().getVariant()), std::sqrt(1.25));
	EXPECT_EQ(row.at("discrete_median").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("continuous_median").asPrimitive(), PropertyValue(2.5));
}

TEST_F(CypherAggregateContractsTest, PreservesEmptyAndAllNullAggregateSemantics) {
	const auto result = execute(
			"UNWIND [null] AS value "
			"RETURN count(value) AS value_count, sum(value) AS total, avg(value) AS average, "
			"min(value) AS minimum, max(value) AS maximum, collect(value) AS values, "
			"stdev(value) AS sample_stdev, stdevp(value) AS population_stdev, "
			"percentileDisc(value, 0.5) AS discrete_median, percentileCont(value, 0.5) AS continuous_median");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("value_count").asPrimitive(), PropertyValue(0));
	for (const std::string &column: {"total", "average", "minimum", "maximum", "sample_stdev", "population_stdev",
								  "discrete_median", "continuous_median"}) {
		EXPECT_EQ(row.at(column).asPrimitive(), PropertyValue()) << column;
	}
	EXPECT_EQ(row.at("values").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST_F(CypherAggregateContractsTest, AggregatesOptionalMatchNullExtensionsCorrectly) {
	(void) execute(
			"CREATE (:OptionalAggregatePerson {name: 'Alice'})-[:RATED {score: 5}]->"
			"(:OptionalAggregateMovie {title: 'Native storage'})");
	(void) execute("CREATE (:OptionalAggregatePerson {name: 'Bob'})");

	const auto result = execute(
			"MATCH (person:OptionalAggregatePerson) "
			"OPTIONAL MATCH (person)-[rating:RATED]->(movie:OptionalAggregateMovie) "
			"RETURN count(*) AS row_count, count(movie) AS movie_count, "
			"sum(rating.score) AS total_score, collect(movie.title) AS movie_titles");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	// OPTIONAL MATCH emits one NULL-extended row for Bob; expression aggregates omit its NULL values.
	EXPECT_EQ(row.at("row_count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("movie_count").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("total_score").asPrimitive(), PropertyValue(5));
	EXPECT_EQ(row.at("movie_titles").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("Native storage")}));
}

TEST_F(CypherAggregateContractsTest, GroupsAndFiltersOptionalMatchAggregateResults) {
	(void) execute("CREATE (:OptionalAggregateGroup {name: 'Alice', department: 'Engineering'})-[:KNOWS]->(:OptionalAggregateGroup {name: 'Bob', department: 'Engineering'})");
	(void) execute("CREATE (:OptionalAggregateGroup {name: 'Carol', department: 'Sales'})");

	const auto grouped = execute(
			"MATCH (person:OptionalAggregateGroup) "
			"OPTIONAL MATCH (person)-[:KNOWS]->(friend:OptionalAggregateGroup) "
			"RETURN person.department AS department, count(friend) AS friend_count "
			"ORDER BY department ASC");
	ASSERT_EQ(grouped.rowCount(), 2U);
	EXPECT_EQ(grouped.getRows()[0].at("department").asPrimitive(), PropertyValue("Engineering"));
	EXPECT_EQ(grouped.getRows()[0].at("friend_count").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(grouped.getRows()[1].at("department").asPrimitive(), PropertyValue("Sales"));
	EXPECT_EQ(grouped.getRows()[1].at("friend_count").asPrimitive(), PropertyValue(0));

	const auto filtered = execute(
			"MATCH (person:OptionalAggregateGroup) "
			"OPTIONAL MATCH (person)-[:KNOWS]->(friend:OptionalAggregateGroup) "
			"WITH person.name AS name, count(friend) AS friend_count "
			"WHERE friend_count > 0 RETURN name, friend_count");
	ASSERT_EQ(filtered.rowCount(), 1U);
	EXPECT_EQ(filtered.getRows().front().at("name").asPrimitive(), PropertyValue("Alice"));
	EXPECT_EQ(filtered.getRows().front().at("friend_count").asPrimitive(), PropertyValue(1));
}

TEST_F(CypherAggregateContractsTest, DeduplicatesWithBeforeTheNextAggregateStage) {
	(void) execute("CREATE (:WithDistinctAggregate {category: 'A'}), (:WithDistinctAggregate {category: 'A'}), "
				  "(:WithDistinctAggregate {category: 'B'})");

	const auto result = execute(
			"MATCH (node:WithDistinctAggregate) WITH DISTINCT node.category AS category "
			"RETURN count(category) AS category_count");

	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("category_count").asPrimitive(), PropertyValue(2));
}

TEST_F(CypherAggregateContractsTest, SupportsInclusivePercentileEndpoints) {
	const auto result = execute(
			"UNWIND [4, 1, 3, 2] AS value "
			"RETURN percentileDisc(value, 0.0) AS discrete_min, percentileDisc(value, 1.0) AS discrete_max, "
			"percentileCont(value, 0.0) AS continuous_min, percentileCont(value, 1.0) AS continuous_max");

	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("discrete_min").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("discrete_max").asPrimitive(), PropertyValue(4));
	EXPECT_EQ(row.at("continuous_min").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("continuous_max").asPrimitive(), PropertyValue(4));
}

TEST_F(CypherAggregateContractsTest, RejectsNonNumericNumericAggregateInputs) {
	EXPECT_THROW(static_cast<void>(execute("UNWIND ['invalid'] AS value RETURN sum(value)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND [true] AS value RETURN avg(value)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND ['invalid'] AS value RETURN stdev(value)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND [[1, 2]] AS value RETURN stdevp(value)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND ['invalid'] AS value RETURN percentileDisc(value, 0.5)")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND ['invalid'] AS value RETURN percentileCont(value, 0.5)")),
			 std::invalid_argument);
}

TEST_F(CypherAggregateContractsTest, RejectsInvalidPercentileArguments) {
	EXPECT_THROW(static_cast<void>(execute("UNWIND [1, 2] AS value RETURN percentileDisc(value, -0.1)")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND [1, 2] AS value RETURN percentileCont(value, 1.1)")),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("UNWIND [1, 2] AS value RETURN percentileDisc(value)")),
			 std::runtime_error);
	EXPECT_THROW(static_cast<void>(execute("UNWIND [1, 2] AS value RETURN percentileCont(value, 'invalid')")),
			 std::invalid_argument);
}
