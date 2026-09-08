/**
 * @file test_CypherLogicalOnlyParser.cpp
 * @brief Tests format-independent Cypher logical-plan parsing.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "CypherToPlanVisitor.hpp"
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

	TEST(CypherLogicalOnlyParserTest, ClassifiesReadAndDataMutationPlansForCaching) {
		CypherParserImpl parser;

		query::QueryPlan read = parser.parseToLogical("MATCH (n:Person) RETURN n");
		ASSERT_NE(read.root, nullptr);
		EXPECT_FALSE(read.mutatesData);
		EXPECT_FALSE(read.mutatesSchema);
		EXPECT_TRUE(read.cacheable);
		EXPECT_TRUE(read.optimizable);

		query::QueryPlan write = parser.parseToLogical("CREATE (:Person {name: 'Ada'})");
		ASSERT_NE(write.root, nullptr);
		EXPECT_TRUE(write.mutatesData);
		EXPECT_FALSE(write.mutatesSchema);
		EXPECT_TRUE(write.cacheable);
		EXPECT_FALSE(write.optimizable);
	}

	TEST(CypherLogicalOnlyParserTest, ClassifiesSchemaControlAndObservabilityPlansAsNonCacheable) {
		CypherParserImpl parser;

		const auto expectNonCacheable = [&parser](const std::string &cypher, query::logical::LogicalOpType type) {
			SCOPED_TRACE(cypher);
			query::QueryPlan plan = parser.parseToLogical(cypher);
			EXPECT_NE(plan.root, nullptr);
			if (plan.root != nullptr) {
				EXPECT_EQ(plan.root->getType(), type);
			}
			EXPECT_FALSE(plan.cacheable);
			EXPECT_FALSE(plan.optimizable);
			return plan;
		};

		const query::QueryPlan scalarIndex =
				expectNonCacheable("CREATE INDEX ON :Person(name)", query::logical::LogicalOpType::LOP_CREATE_INDEX);
		EXPECT_TRUE(scalarIndex.mutatesSchema);
		const query::QueryPlan droppedIndex =
				expectNonCacheable("DROP INDEX person_name", query::logical::LogicalOpType::LOP_DROP_INDEX);
		EXPECT_TRUE(droppedIndex.mutatesSchema);
		const query::QueryPlan shownIndexes =
				expectNonCacheable("SHOW INDEXES", query::logical::LogicalOpType::LOP_SHOW_INDEXES);
		EXPECT_FALSE(shownIndexes.mutatesSchema);
		const query::QueryPlan vectorIndex = expectNonCacheable(
				"CREATE VECTOR INDEX person_embedding ON :Person(embedding) OPTIONS {dimension: 2}",
				query::logical::LogicalOpType::LOP_CREATE_VECTOR_INDEX);
		EXPECT_TRUE(vectorIndex.mutatesSchema);
		const query::QueryPlan constraint = expectNonCacheable(
				"CREATE CONSTRAINT person_email FOR (n:Person) REQUIRE n.email IS UNIQUE",
				query::logical::LogicalOpType::LOP_CREATE_CONSTRAINT);
		EXPECT_TRUE(constraint.mutatesSchema);
		const query::QueryPlan droppedConstraint =
				expectNonCacheable("DROP CONSTRAINT person_email", query::logical::LogicalOpType::LOP_DROP_CONSTRAINT);
		EXPECT_TRUE(droppedConstraint.mutatesSchema);
		const query::QueryPlan transaction =
				expectNonCacheable("BEGIN", query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL);
		EXPECT_FALSE(transaction.mutatesData);
		EXPECT_FALSE(transaction.mutatesSchema);
		const query::QueryPlan explain =
				expectNonCacheable("EXPLAIN RETURN 1 AS value", query::logical::LogicalOpType::LOP_EXPLAIN);
		EXPECT_FALSE(explain.mutatesData);
		EXPECT_FALSE(explain.mutatesSchema);
		const query::QueryPlan profile =
				expectNonCacheable("PROFILE RETURN 1 AS value", query::logical::LogicalOpType::LOP_PROFILE);
		EXPECT_FALSE(profile.mutatesData);
		EXPECT_FALSE(profile.mutatesSchema);
	}

	TEST(CypherLogicalOnlyParserTest, BuildsPlansForPublicClauseAndSubqueryForms) {
		CypherParserImpl parser;
		const auto expectPlan = [&parser](const std::string &cypher) {
			SCOPED_TRACE(cypher);
			query::QueryPlan plan = parser.parseToLogical(cypher);
			ASSERT_NE(plan.root, nullptr);
		};

		expectPlan("RETURN 1 AS value UNION RETURN 2 AS value");
		expectPlan("RETURN 1 AS value UNION ALL RETURN 2 AS value");
		expectPlan("UNWIND [1, 2] AS value RETURN value");
		expectPlan("MATCH (n:Person) WITH n RETURN n");
		expectPlan("MATCH (n:Person) SET n.name = 'Ada'");
		expectPlan("MATCH (n:Person) DELETE n");
		expectPlan("MATCH (n:Person) REMOVE n.name");
		expectPlan("MERGE (:Person {name: 'Ada'})");
		expectPlan("CREATE INDEX person_name FOR (n:Person) ON (n.name)");
		expectPlan("DROP INDEX ON :Person(name)");
		expectPlan("CREATE CONSTRAINT edge_weight FOR ()-[r:REL]-() REQUIRE r.weight IS NOT NULL");
		expectPlan("DROP CONSTRAINT edge_weight IF EXISTS");
		expectPlan("COMMIT");
		expectPlan("ROLLBACK");
		expectPlan("FOREACH (value IN [1, 2] | CREATE (:Item {value: value}))");
		expectPlan("CALL { RETURN 1 AS value } RETURN value");
		expectPlan("MATCH (n:Person) CALL { WITH n RETURN n } RETURN n");
		expectPlan("CALL { WITH 1 AS seed RETURN seed } IN TRANSACTIONS OF 2 ROWS RETURN 1");
		expectPlan("LOAD CSV WITH HEADERS FROM 'file:///people.csv' AS row FIELDTERMINATOR '\\t' RETURN row");
		expectPlan("LOAD CSV FROM 'file:///people.csv' AS row RETURN row");
		expectPlan("CALL dbms.listConfig()");
		expectPlan("CALL dbms.listConfig() YIELD key RETURN key");
		expectPlan("MATCH (n:Person) CALL { WITH * RETURN n } RETURN n");
		expectPlan("MATCH (n:Person) CALL { WITH n CREATE (:Copy {name: n.name}) } RETURN n");
		expectPlan("CALL { WITH 1 AS seed RETURN seed } IN TRANSACTIONS RETURN 1");
	}

	TEST(CypherLogicalOnlyParserTest, ReturnsNoPlanBeforeTheVisitorHasProcessedAStatement) {
		CypherToPlanVisitor visitor;
		EXPECT_EQ(visitor.getLogicalPlan(), nullptr);
	}

} // namespace graph::parser::cypher
