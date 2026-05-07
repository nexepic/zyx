/**
 * @file test_ApiSequentialExecution.cpp
 * @brief Tests for sequential query execution through the public C++ API.
 *        Verifies parser reuse via db->execute() across many calls.
 **/

#include <gtest/gtest.h>
#include <string>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

class ApiSequentialExecutionTest : public CppApiTest {};

TEST_F(ApiSequentialExecutionTest, TwentySequentialAutoCommitQueries) {
	// Create nodes
	for (int i = 0; i < 5; ++i) {
		auto r = db->execute("CREATE (n:Item {id: " + std::to_string(i) + ", name: 'item" + std::to_string(i) + "'})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// Verify count
	auto r1 = db->execute("MATCH (n:Item) RETURN count(n) AS cnt");
	ASSERT_TRUE(r1.isSuccess()) << r1.getError();
	ASSERT_TRUE(r1.hasNext());
	r1.next();
	EXPECT_EQ(std::get<int64_t>(r1.get("cnt")), 5);

	// Update properties
	auto r2 = db->execute("MATCH (n:Item) WHERE n.id = 0 SET n.name = 'updated'");
	ASSERT_TRUE(r2.isSuccess()) << r2.getError();

	// Verify update
	auto r3 = db->execute("MATCH (n:Item) WHERE n.name = 'updated' RETURN n.id AS id");
	ASSERT_TRUE(r3.isSuccess()) << r3.getError();
	ASSERT_TRUE(r3.hasNext());
	r3.next();
	EXPECT_EQ(std::get<int64_t>(r3.get("id")), 0);

	// Delete one
	auto r4 = db->execute("MATCH (n:Item) WHERE n.id = 4 DELETE n");
	ASSERT_TRUE(r4.isSuccess()) << r4.getError();

	// Create edges
	auto r5 = db->execute("MATCH (a:Item {id: 0}), (b:Item {id: 1}) CREATE (a)-[:NEXT]->(b)");
	ASSERT_TRUE(r5.isSuccess()) << r5.getError();

	// Traverse
	auto r6 = db->execute("MATCH (a:Item)-[:NEXT]->(b:Item) RETURN a.id AS src, b.id AS dst");
	ASSERT_TRUE(r6.isSuccess()) << r6.getError();
	ASSERT_TRUE(r6.hasNext());
	r6.next();
	EXPECT_EQ(std::get<int64_t>(r6.get("src")), 0);
	EXPECT_EQ(std::get<int64_t>(r6.get("dst")), 1);

	// Final count (should be 4 after deletion)
	auto r7 = db->execute("MATCH (n:Item) RETURN count(n) AS cnt");
	ASSERT_TRUE(r7.isSuccess()) << r7.getError();
	ASSERT_TRUE(r7.hasNext());
	r7.next();
	EXPECT_EQ(std::get<int64_t>(r7.get("cnt")), 4);

	// Additional queries to reach 20+
	for (int i = 10; i < 18; ++i) {
		auto r = db->execute("CREATE (n:Extra {val: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	auto r8 = db->execute("MATCH (n:Extra) RETURN count(n) AS cnt");
	ASSERT_TRUE(r8.isSuccess()) << r8.getError();
	ASSERT_TRUE(r8.hasNext());
	r8.next();
	EXPECT_EQ(std::get<int64_t>(r8.get("cnt")), 8);
}

TEST_F(ApiSequentialExecutionTest, SequentialAlternatingReadWrite) {
	for (int i = 0; i < 15; ++i) {
		SCOPED_TRACE("Iteration " + std::to_string(i));

		auto w = db->execute("CREATE (n:Seq {step: " + std::to_string(i) + "})");
		ASSERT_TRUE(w.isSuccess()) << w.getError();

		auto r = db->execute("MATCH (n:Seq) RETURN count(n) AS cnt");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
		ASSERT_TRUE(r.hasNext());
		r.next();
		EXPECT_EQ(std::get<int64_t>(r.get("cnt")), i + 1);
	}
}

TEST_F(ApiSequentialExecutionTest, ExecuteErrorThenContinue) {
	// 5 valid queries
	for (int i = 0; i < 5; ++i) {
		auto r = db->execute("CREATE (n:V {i: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// 1 invalid query — parser must recover
	auto bad = db->execute("MATC (n) RETRUN n");
	EXPECT_FALSE(bad.isSuccess());

	// 5 more valid queries
	for (int i = 5; i < 10; ++i) {
		auto r = db->execute("CREATE (n:V {i: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	auto cnt = db->execute("MATCH (n:V) RETURN count(n) AS cnt");
	ASSERT_TRUE(cnt.isSuccess());
	ASSERT_TRUE(cnt.hasNext());
	cnt.next();
	EXPECT_EQ(std::get<int64_t>(cnt.get("cnt")), 10);
}

TEST_F(ApiSequentialExecutionTest, MultipleConsecutiveErrorsThenSuccess) {
	auto r1 = db->execute("!!!invalid!!!");
	EXPECT_FALSE(r1.isSuccess());

	auto r2 = db->execute("MATC (n)");
	EXPECT_FALSE(r2.isSuccess());

	auto r3 = db->execute("CREAT (n)");
	EXPECT_FALSE(r3.isSuccess());

	// Valid query must succeed after errors
	auto r4 = db->execute("CREATE (n:OK {val: 1})");
	ASSERT_TRUE(r4.isSuccess()) << r4.getError();

	auto r5 = db->execute("MATCH (n:OK) RETURN n.val AS v");
	ASSERT_TRUE(r5.isSuccess()) << r5.getError();
	ASSERT_TRUE(r5.hasNext());
	r5.next();
	EXPECT_EQ(std::get<int64_t>(r5.get("v")), 1);
}

TEST_F(ApiSequentialExecutionTest, AutoCommitInterleavedWithExplicitTxn) {
	// Auto-commit CREATE
	auto r1 = db->execute("CREATE (n:Auto {val: 1})");
	ASSERT_TRUE(r1.isSuccess()) << r1.getError();

	// Explicit transaction
	{
		auto tx = db->beginTransaction();
		auto tr = tx.execute("CREATE (n:Explicit {val: 2})");
		ASSERT_TRUE(tr.isSuccess()) << tr.getError();
		tx.commit();
	}

	// Auto-commit reads
	auto r2 = db->execute("MATCH (n:Auto) RETURN count(n) AS cnt");
	ASSERT_TRUE(r2.isSuccess());
	ASSERT_TRUE(r2.hasNext());
	r2.next();
	EXPECT_EQ(std::get<int64_t>(r2.get("cnt")), 1);

	auto r3 = db->execute("MATCH (n:Explicit) RETURN count(n) AS cnt");
	ASSERT_TRUE(r3.isSuccess());
	ASSERT_TRUE(r3.hasNext());
	r3.next();
	EXPECT_EQ(std::get<int64_t>(r3.get("cnt")), 1);
}

TEST_F(ApiSequentialExecutionTest, SequentialAggregationQueries) {
	// Build 10-node graph
	for (int i = 0; i < 10; ++i) {
		auto r = db->execute("CREATE (n:Num {val: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// 10 sequential aggregation queries
	for (int i = 0; i < 10; ++i) {
		SCOPED_TRACE("Agg query #" + std::to_string(i));
		auto r = db->execute("MATCH (n:Num) RETURN count(n) AS cnt");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
		ASSERT_TRUE(r.hasNext());
		r.next();
		EXPECT_EQ(std::get<int64_t>(r.get("cnt")), 10);
	}
}
