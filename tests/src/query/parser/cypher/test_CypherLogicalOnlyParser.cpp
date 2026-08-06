/**
 * @file test_CypherLogicalOnlyParser.cpp
 * @brief Tests format-independent Cypher logical-plan parsing.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

namespace graph::parser::cypher {

	TEST(CypherLogicalOnlyParserTest, BuildsAReadOnlyLogicalPlan) {
		CypherParserImpl parser;
		query::QueryPlan plan = parser.parseToLogical("MATCH (n:Person {age: 42}) RETURN n.age AS age LIMIT 1");

		ASSERT_NE(plan.root, nullptr);
		EXPECT_FALSE(plan.mutatesData);
		EXPECT_FALSE(plan.mutatesSchema);
		EXPECT_EQ(plan.root->getType(), query::logical::LogicalOpType::LOP_LIMIT);
	}

} // namespace graph::parser::cypher
