/**
 * @file test_ApiTransactionEdgeCases.cpp
 * @brief Tests for transaction edge cases: sequential txns, long txns,
 *        double-commit, rollback-after-commit, etc.
 **/

#include <gtest/gtest.h>
#include <string>
#include <variant>

#include "ApiTestFixture.hpp"

class ApiTransactionEdgeCasesTest : public CppApiTest {};

TEST_F(ApiTransactionEdgeCasesTest, MultipleSuccessiveTransactions) {
	for (int i = 0; i < 5; ++i) {
		auto tx = db->beginTransaction();
		auto r = tx.execute("CREATE (n:Txn {round: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
		tx.commit();
	}

	auto r = db->execute("MATCH (n:Txn) RETURN count(n) AS cnt");
	ASSERT_TRUE(r.isSuccess()) << r.getError();
	ASSERT_TRUE(r.hasNext());
	r.next();
	EXPECT_EQ(std::get<int64_t>(r.get("cnt")), 5);
}

TEST_F(ApiTransactionEdgeCasesTest, RollbackThenCommitSequence) {
	{
		auto tx = db->beginTransaction();
		tx.execute("CREATE (n:Ghost {val: 1})");
		tx.rollback();
	}

	{
		auto tx = db->beginTransaction();
		tx.execute("CREATE (n:Real {val: 2})");
		tx.commit();
	}

	auto r1 = db->execute("MATCH (n:Ghost) RETURN count(n) AS cnt");
	ASSERT_TRUE(r1.isSuccess());
	ASSERT_TRUE(r1.hasNext());
	r1.next();
	EXPECT_EQ(std::get<int64_t>(r1.get("cnt")), 0);

	auto r2 = db->execute("MATCH (n:Real) RETURN count(n) AS cnt");
	ASSERT_TRUE(r2.isSuccess());
	ASSERT_TRUE(r2.hasNext());
	r2.next();
	EXPECT_EQ(std::get<int64_t>(r2.get("cnt")), 1);
}

TEST_F(ApiTransactionEdgeCasesTest, TransactionWithTenCreates) {
	auto tx = db->beginTransaction();
	for (int i = 0; i < 10; ++i) {
		auto r = tx.execute("CREATE (n:Bulk {id: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}
	tx.commit();

	auto r = db->execute("MATCH (n:Bulk) RETURN count(n) AS cnt");
	ASSERT_TRUE(r.isSuccess());
	ASSERT_TRUE(r.hasNext());
	r.next();
	EXPECT_EQ(std::get<int64_t>(r.get("cnt")), 10);
}

TEST_F(ApiTransactionEdgeCasesTest, TransactionWithMixedOperations) {
	auto tx = db->beginTransaction();

	// CREATE 3 nodes
	tx.execute("CREATE (n:Mix {id: 1, val: 'a'})");
	tx.execute("CREATE (n:Mix {id: 2, val: 'b'})");
	tx.execute("CREATE (n:Mix {id: 3, val: 'c'})");

	// SET 2 properties
	tx.execute("MATCH (n:Mix) WHERE n.id = 1 SET n.val = 'updated'");
	tx.execute("MATCH (n:Mix) WHERE n.id = 2 SET n.val = 'updated'");

	// DELETE 1 node
	tx.execute("MATCH (n:Mix) WHERE n.id = 3 DELETE n");

	// Verify within txn
	auto r = tx.execute("MATCH (n:Mix) RETURN count(n) AS cnt");
	ASSERT_TRUE(r.isSuccess()) << r.getError();
	ASSERT_TRUE(r.hasNext());
	r.next();
	EXPECT_EQ(std::get<int64_t>(r.get("cnt")), 2);

	tx.commit();

	// Verify after commit
	auto r2 = db->execute("MATCH (n:Mix) WHERE n.val = 'updated' RETURN count(n) AS cnt");
	ASSERT_TRUE(r2.isSuccess());
	ASSERT_TRUE(r2.hasNext());
	r2.next();
	EXPECT_EQ(std::get<int64_t>(r2.get("cnt")), 2);
}

TEST_F(ApiTransactionEdgeCasesTest, AutoCommitReadDuringActiveTxn) {
	// Seed data
	auto seed = db->execute("CREATE (n:Seed {val: 42})");
	ASSERT_TRUE(seed.isSuccess()) << seed.getError();

	auto tx = db->beginTransaction();
	tx.execute("CREATE (n:InTxn {val: 99})");

	// Auto-commit read while txn is active — should not crash
	auto r = db->execute("MATCH (n:Seed) RETURN n.val AS v");
	ASSERT_TRUE(r.isSuccess()) << r.getError();
	ASSERT_TRUE(r.hasNext());
	r.next();
	EXPECT_EQ(std::get<int64_t>(r.get("v")), 42);

	tx.commit();
}

TEST_F(ApiTransactionEdgeCasesTest, CommitAlreadyCommittedThrows) {
	auto tx = db->beginTransaction();
	tx.execute("CREATE (n:X {v: 1})");
	tx.commit();

	EXPECT_THROW(tx.commit(), std::runtime_error);
}

TEST_F(ApiTransactionEdgeCasesTest, RollbackAfterCommitThrows) {
	auto tx = db->beginTransaction();
	tx.execute("CREATE (n:Y {v: 2})");
	tx.commit();

	EXPECT_THROW(tx.rollback(), std::runtime_error);
}

TEST_F(ApiTransactionEdgeCasesTest, ExecuteAfterRollbackReturnsError) {
	auto tx = db->beginTransaction();
	tx.execute("CREATE (n:Z {v: 3})");
	tx.rollback();

	auto r = tx.execute("MATCH (n) RETURN n");
	EXPECT_FALSE(r.isSuccess());
}
