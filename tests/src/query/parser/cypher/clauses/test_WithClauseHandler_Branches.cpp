/**
 * @file test_WithClauseHandler_Branches.cpp
 * @brief Focused branch tests for WithClauseHandler.
 */

#include "QueryTestFixture.hpp"

class WithClauseHandlerBranchesTest : public QueryTestFixture {};

TEST_F(WithClauseHandlerBranchesTest, WithAsFirstClauseThrows) {
	EXPECT_THROW((void)execute("WITH 1 AS x RETURN x"), std::runtime_error);
}

TEST_F(WithClauseHandlerBranchesTest, WithNonAggregateFunctionFallsBackToProject) {
	(void)execute("CREATE (n:WBranch {value: 10})");
	auto result = execute("MATCH (n:WBranch) WITH toString(n.value) AS s RETURN s");

	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows()[0].at("s").toString(), "10");
}

TEST_F(WithClauseHandlerBranchesTest, WithCountStarCoversZeroArgumentAggregatePath) {
	(void)execute("CREATE (n:WCount)");
	(void)execute("CREATE (n:WCount)");

	auto result = execute("MATCH (n:WCount) WITH count(*) AS c RETURN c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getRows()[0].at("c").toString(), "2");
}
