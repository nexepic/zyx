/**
 * @file test_AdminClauseHandler_EdgeConstraint.cpp
 * @brief Focused edge-constraint branch tests for AdminClauseHandler.
 */

#include "QueryTestFixture.hpp"

class AdminClauseHandlerEdgeConstraintTest : public QueryTestFixture {};

TEST_F(AdminClauseHandlerEdgeConstraintTest, CreateEdgeUniqueConstraint_ParsesAndExecutes) {
	auto result = execute("CREATE CONSTRAINT uniq_since FOR ()-[r:KNOWS]-() REQUIRE r.since IS UNIQUE");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto show = execute("SHOW CONSTRAINT");
	EXPECT_GE(show.rowCount(), 1UL);
}

TEST_F(AdminClauseHandlerEdgeConstraintTest, CreateEdgePropertyTypeConstraint_ParsesAndExecutes) {
	auto result = execute("CREATE CONSTRAINT type_since FOR ()-[r:KNOWS]-() REQUIRE r.since IS ::INTEGER");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto show = execute("SHOW CONSTRAINT");
	EXPECT_GE(show.rowCount(), 1UL);
}
