/**
 * @file test_QueryEnginePublicContracts.cpp
 * @brief Public database contracts that exercise the native QueryEngine boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "QueryTestFixture.hpp"

namespace graph::query {

	TEST_F(QueryTestFixture, QueryEngineExecutesSchemaReadsAndCallerTraversalLimits) {
		const QueryResult createIndex = execute("CREATE INDEX public_person_name FOR (n:PublicPerson) ON (n.name)");
		ASSERT_EQ(createIndex.rowCount(), 1U);

		const QueryResult indexes = execute("SHOW INDEXES");
		ASSERT_EQ(indexes.rowCount(), 1U);
		EXPECT_EQ(indexes.getRows().front().at("name").toString(), "public_person_name");

		ASSERT_EQ(execute("CREATE (:PublicDepthStart)-[:PUBLIC_STEP]->(:PublicDepthMiddle)-[:PUBLIC_STEP]->(:PublicDepthEnd)")
					  .rowCount(),
				  0U);
		QueryContext oneHopContext;
		oneHopContext.maxVarLengthDepth = 1;
		const QueryResult bounded = db->getQueryEngine()->execute(
				"MATCH (:PublicDepthStart)-[:PUBLIC_STEP*1..2]->(end:PublicDepthEnd) RETURN end", oneHopContext);
		EXPECT_TRUE(bounded.isEmpty());

		const QueryResult unbounded =
				execute("MATCH (:PublicDepthStart)-[:PUBLIC_STEP*1..2]->(end:PublicDepthEnd) RETURN end");
		EXPECT_EQ(unbounded.rowCount(), 1U);
	}

	TEST_F(QueryTestFixture, QueryEngineRejectsSchemaChangesInReadWriteTransactionMode) {
		QueryContext context;
		context.execMode = ExecMode::EM_READ_WRITE;

		// Schema publication is an autocommit-only operation. A caller-owned
		// read-write transaction must fail before it can change the catalog.
		EXPECT_THROW(db->getQueryEngine()->execute(
					 "CREATE INDEX rejected_schema_change FOR (n:RejectedSchemaChange) ON (n.name)", context),
				 std::runtime_error);
		EXPECT_TRUE(execute("SHOW INDEXES").isEmpty());
	}

} // namespace graph::query
