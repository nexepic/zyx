/**
 * @file test_ApiQueryClassification.cpp
 * @brief Coverage-focused tests for DatabaseImpl query classification paths.
 */

#include "ApiTestFixture.hpp"

TEST_F(CppApiTest, EmptyAndWhitespaceQueryReturnErrors) {
	auto empty = db->execute("");
	EXPECT_FALSE(empty.isSuccess());
	EXPECT_FALSE(empty.getError().empty());

	auto spaces = db->execute("   \t  ");
	EXPECT_FALSE(spaces.isSuccess());
	EXPECT_FALSE(spaces.getError().empty());
}

TEST_F(CppApiTest, ExplainAndProfilePrefixesAreHandled) {
	db->createNode("QClass", {{"v", (int64_t) 1}});
	db->save();

	auto explainRes = db->execute("EXPLAIN MATCH (n:QClass) RETURN n.v");
	EXPECT_TRUE(explainRes.isSuccess());

	auto profileRes = db->execute("PROFILE MATCH (n:QClass) RETURN n.v");
	EXPECT_TRUE(profileRes.isSuccess());
}

TEST_F(CppApiTest, CallTokenInNonLeadingPositionIsHandledSafely) {
	// Non-leading CALL token path (string literal still triggers conservative fallback)
	auto mixedRes = db->execute("RETURN 'CALL' AS marker");
	EXPECT_TRUE(mixedRes.isSuccess());
}

TEST_F(CppApiTest, CypherTransactionStatementsToggleActiveState) {
	EXPECT_FALSE(db->hasActiveTransaction());

	auto beginRes = db->execute("   BEGIN");
	EXPECT_TRUE(beginRes.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	auto commitRes = db->execute("COMMIT");
	EXPECT_TRUE(commitRes.isSuccess());
	EXPECT_FALSE(db->hasActiveTransaction());

	auto rollbackWithoutTxn = db->execute("ROLLBACK");
	EXPECT_FALSE(rollbackWithoutTxn.isSuccess());
	EXPECT_FALSE(rollbackWithoutTxn.getError().empty());
}
