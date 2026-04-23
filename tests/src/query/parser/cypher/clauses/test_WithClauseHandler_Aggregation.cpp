/**
 * @file test_WithClauseHandler_Aggregation.cpp
 * @brief Focused branch tests for WithClauseHandler.
 */

#include "QueryTestFixture.hpp"

class WithClauseHandlerAggregationTest : public QueryTestFixture {};

TEST_F(WithClauseHandlerAggregationTest, WithAsFirstClauseThrows) {
	EXPECT_THROW((void)execute("WITH 1 AS x RETURN x"), std::runtime_error);
}

TEST_F(WithClauseHandlerAggregationTest, WithNonAggregateFunctionFallsBackToProject) {
	(void)execute("CREATE (n:WBranch {value: 10})");
	auto result = execute("MATCH (n:WBranch) WITH toString(n.value) AS s RETURN s");

	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows()[0].at("s").toString(), "10");
}

TEST_F(WithClauseHandlerAggregationTest, WithCountStarCoversZeroArgumentAggregatePath) {
	(void)execute("CREATE (n:WCount)");
	(void)execute("CREATE (n:WCount)");

	auto result = execute("MATCH (n:WCount) WITH count(*) AS c RETURN c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows()[0].at("c").toString(), "2");
}
