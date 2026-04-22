/**
 * @file test_ApiDatabaseImpl_Branches.cpp
 * @brief Focused branch tests for DatabaseImpl conversion/txn paths.
 */

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "ApiTestFixture.hpp"

TEST_F(CppApiTest, VectorFloatPropertyConversionRoundTrip) {
	std::unordered_map<std::string, zyx::Value> props;
	props["emb"] = std::vector<float>{1.0F, 2.5F, -3.25F};

	db->createNode("VecFloatNode", props);
	db->save();

	auto res = db->execute("MATCH (n:VecFloatNode) RETURN n.emb");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.emb");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	const auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 3UL);
	EXPECT_NE(vec[0].find("1"), std::string::npos);
	EXPECT_NE(vec[1].find("2.5"), std::string::npos);
	EXPECT_NE(vec[2].find("-3.25"), std::string::npos);
}

TEST_F(CppApiTest, StringVectorParsingCoversIntDoubleAndFallbackBranches) {
	std::unordered_map<std::string, zyx::Value> props;
	props["mixed"] = std::vector<std::string>{"42", "3.14", "12x", "hello"};

	db->createNode("VecStringNode", props);
	db->save();

	auto res = db->execute("MATCH (n:VecStringNode) RETURN n.mixed");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.mixed");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	const auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 4UL);
	EXPECT_EQ(vec[0], "42");
	EXPECT_NE(vec[1].find("3.14"), std::string::npos);
	EXPECT_EQ(vec[2], "12x");
	EXPECT_EQ(vec[3], "hello");
}

TEST_F(CppApiTest, ExecuteParamsCoversTxnDelegateAndAutoCommitBranches) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	auto commitRes = db->execute("COMMIT", {{"unused", (int64_t) 1}});
	EXPECT_TRUE(commitRes.isSuccess());
	EXPECT_FALSE(db->hasActiveTransaction());

	auto writeRes = db->execute(
		"CREATE (n:ParamBranch {x: $x}) RETURN n.x",
		{{"x", (int64_t) 7}}
	);
	EXPECT_TRUE(writeRes.isSuccess());

	auto readRes = db->execute(
		"RETURN $x",
		{{"x", (int64_t) 7}}
	);
	EXPECT_TRUE(readRes.isSuccess());
	EXPECT_TRUE(readRes.hasNext());

	auto verifyRes = db->execute("MATCH (n:ParamBranch) RETURN n.x");
	EXPECT_TRUE(verifyRes.isSuccess());
}

TEST_F(CppApiTest, CreateNodeWithMultipleLabelsCoversImplicitAndExplicitTxnPaths) {
	db->createNode(std::vector<std::string>{"L1", "L2"}, {{"k", (int64_t) 1}});
	db->save();

	auto labelQuery = db->execute("MATCH (n:L1:L2) RETURN n.k");
	ASSERT_TRUE(labelQuery.hasNext());
	labelQuery.next();
	auto k = labelQuery.get("n.k");
	ASSERT_TRUE(std::holds_alternative<int64_t>(k));
	EXPECT_EQ(std::get<int64_t>(k), 1);

	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_NO_THROW(db->createNode(std::vector<std::string>{}, {}));
	txn.commit();

	auto anyNode = db->execute("MATCH (n) RETURN n");
	EXPECT_TRUE(anyNode.isSuccess());
}

// ============================================================================
// Batch methods inside a Cypher-level BEGIN transaction
// Covers: needsImplicitTransaction() returning false via cypherTxn_.has_value()
// ============================================================================

TEST_F(CppApiTest, BatchMethodsInsideCypherTransaction) {
	// Start a Cypher-level transaction via execute("BEGIN")
	auto beginRes = db->execute("BEGIN");
	ASSERT_TRUE(beginRes.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	// All batch write methods should participate in the Cypher transaction
	// (no implicit transaction created — needsImplicitTransaction() == false)
	EXPECT_NO_THROW(db->createNode("CypherTxnNode", {{"val", (int64_t) 1}}));
	EXPECT_NO_THROW(db->createNode(std::vector<std::string>{"MultiA", "MultiB"}, {{"val", (int64_t) 2}}));
	EXPECT_NO_THROW(db->createNodes("BatchNode", {{{"val", (int64_t) 3}}, {{"val", (int64_t) 4}}}));
	int64_t nodeId = db->createNode("RetIdNode", {{"val", (int64_t) 5}});
	EXPECT_GT(nodeId, 0);
	int64_t nodeId2 = db->createNode("RetIdNode", {{"val", (int64_t) 6}});
	EXPECT_NO_THROW(db->createEdge(nodeId, nodeId2, "LINK", {{"w", (int64_t) 10}}));

	// Rollback — all batch changes should be undone
	auto rollbackRes = db->execute("ROLLBACK");
	ASSERT_TRUE(rollbackRes.isSuccess());
	EXPECT_FALSE(db->hasActiveTransaction());

	// Verify nothing was persisted
	auto countRes = db->execute("MATCH (n) RETURN count(n)");
	ASSERT_TRUE(countRes.hasNext());
	countRes.next();
	auto count = std::get<int64_t>(countRes.get(0));
	EXPECT_EQ(count, 0);
}

// ============================================================================
// Auto-open: batch methods on a not-yet-opened database (ensureOpen)
// ============================================================================

TEST_F(CppApiTest, BatchMethodsAutoOpenDatabase) {
	// Create a fresh Database that has NOT been opened
	auto tempDir = fs::temp_directory_path();
	auto freshPath = (tempDir / ("auto_open_test_" + std::to_string(std::rand()))).string();
	auto freshDb = std::make_unique<zyx::Database>(freshPath);
	// Do NOT call freshDb->open()

	// createNode should auto-open the database via ensureOpen()
	EXPECT_NO_THROW(freshDb->createNode("AutoOpen", {{"k", (int64_t) 1}}));

	// Verify the data was written
	auto res = freshDb->execute("MATCH (n:AutoOpen) RETURN n.k");
	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_EQ(std::get<int64_t>(res.get("n.k")), 1);

	freshDb->close();
	std::error_code ec;
	fs::remove_all(freshPath, ec);
	fs::remove(freshPath + "-wal", ec);
}

TEST_F(CppApiTest, UtilityMethodsSaveThreadPoolAndTokenBoundaryQuery) {
	EXPECT_NO_THROW(db->setThreadPoolSize(2));
	EXPECT_NO_THROW(db->save());

	auto boundaryRes = db->execute("RETURN 'CREATED' AS token");
	EXPECT_TRUE(boundaryRes.isSuccess());
}
