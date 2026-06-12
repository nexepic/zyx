/**
 * @file test_ApiTransaction.cpp
 * @author Nexepic
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <variant>

#include "ApiTestFixture.hpp"

// ============================================================================
// Transaction API Tests
// ============================================================================

TEST_F(CppApiTest, TransactionBeginCommit) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnTest {name: 'committed'})");
	EXPECT_TRUE(res.isSuccess());

	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto queryRes = db->execute("MATCH (n:TxnTest) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "committed");
}

TEST_F(CppApiTest, BulkTransactionSupportsDirectBatchWrites) {
	auto txn = db->beginBulkTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_FALSE(txn.isReadOnly());

	auto ids = db->createNodes("BulkApiUser",
							   {{{"id", std::string("u1")}, {"score", int64_t{1}}},
								{{"id", std::string("u2")}, {"score", int64_t{2}}}});
	ASSERT_EQ(ids.size(), 2U);
	auto edgeIds = db->createEdges("FOLLOWS", {{ids[0], ids[1], {{"weight", int64_t{7}}}}});
	ASSERT_EQ(edgeIds.size(), 1U);

	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto result = db->execute("MATCH (:BulkApiUser {id: 'u1'})-[r:FOLLOWS]->(:BulkApiUser {id: 'u2'}) RETURN r.weight");
	ASSERT_TRUE(result.isSuccess());
	ASSERT_TRUE(result.hasNext());
	result.next();
	auto weight = result.get("r.weight");
	ASSERT_TRUE(std::holds_alternative<int64_t>(weight));
	EXPECT_EQ(std::get<int64_t>(weight), 7);
}

TEST_F(CppApiTest, RowWiseBatchInsertHandlesHomogeneousAndSparseProperties) {
	auto homogeneousIds = db->createNodes("RowWiseHomogeneousUser",
										  {{{"id", std::string("h1")}, {"score", int64_t{10}}},
										   {{"id", std::string("h2")}, {"score", int64_t{20}}}});
	ASSERT_EQ(homogeneousIds.size(), 2U);

	auto sparseIds = db->createNodes("RowWiseSparseUser",
									 {{{"id", std::string("s1")}, {"score", int64_t{30}}},
									  {{"id", std::string("s2")}}});
	ASSERT_EQ(sparseIds.size(), 2U);

	auto homogeneousEdges = db->createEdges(
			"ROW_WISE_HOMOGENEOUS_EDGE",
			{{homogeneousIds[0], homogeneousIds[1], {{"weight", int64_t{1}}, {"rank", int64_t{2}}}},
			 {homogeneousIds[1], homogeneousIds[0], {{"weight", int64_t{3}}, {"rank", int64_t{4}}}}});
	ASSERT_EQ(homogeneousEdges.size(), 2U);

	auto sparseEdges = db->createEdges("ROW_WISE_SPARSE_EDGE",
									   {{sparseIds[0], sparseIds[1], {{"weight", int64_t{5}}}},
										{sparseIds[1], sparseIds[0], {}}});
	ASSERT_EQ(sparseEdges.size(), 2U);

	auto homogeneousResult = db->execute(
			"MATCH (:RowWiseHomogeneousUser {id: 'h1'})-[r:ROW_WISE_HOMOGENEOUS_EDGE]->"
			"(n:RowWiseHomogeneousUser {id: 'h2'}) RETURN r.rank, n.score");
	ASSERT_TRUE(homogeneousResult.isSuccess());
	ASSERT_TRUE(homogeneousResult.hasNext());
	homogeneousResult.next();
	auto rank = homogeneousResult.get("r.rank");
	auto score = homogeneousResult.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(rank));
	ASSERT_TRUE(std::holds_alternative<int64_t>(score));
	EXPECT_EQ(std::get<int64_t>(rank), 2);
	EXPECT_EQ(std::get<int64_t>(score), 20);

	auto sparseResult = db->execute("MATCH (n:RowWiseSparseUser {id: 's2'}) RETURN n.id");
	ASSERT_TRUE(sparseResult.isSuccess());
	ASSERT_TRUE(sparseResult.hasNext());
	sparseResult.next();
	auto id = sparseResult.get("n.id");
	ASSERT_TRUE(std::holds_alternative<std::string>(id));
	EXPECT_EQ(std::get<std::string>(id), "s2");
}

TEST_F(CppApiTest, ColumnarBulkInsertCreatesNodesAndEdges) {
	auto txn = db->beginBulkTransaction();

	const std::vector<zyx::PropertyColumn> nodeColumns{
			{"id", {std::string("c1"), std::string("c2"), std::string("c3")}},
			{"score", {int64_t{10}, int64_t{20}, int64_t{30}}},
			{"active", {true, false, true}},
	};
	const auto ids = db->createNodesColumnar("ColumnarUser", 3, nodeColumns);
	ASSERT_EQ(ids.size(), 3U);

	const std::vector<zyx::PropertyColumn> edgeColumns{
			{"weight", {int64_t{4}, int64_t{8}}},
	};
	const auto edgeIds = db->createEdgesColumnar(
			"COLUMNAR_FOLLOWS", {ids[0], ids[1]}, {ids[1], ids[2]}, edgeColumns);
	ASSERT_EQ(edgeIds.size(), 2U);

	txn.commit();

	auto result = db->execute(
			"MATCH (:ColumnarUser {id: 'c1'})-[r:COLUMNAR_FOLLOWS]->(n:ColumnarUser {id: 'c2'}) "
			"RETURN r.weight, n.score");
	ASSERT_TRUE(result.isSuccess());
	ASSERT_TRUE(result.hasNext());
	result.next();
	auto weight = result.get("r.weight");
	auto score = result.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(weight));
	ASSERT_TRUE(std::holds_alternative<int64_t>(score));
	EXPECT_EQ(std::get<int64_t>(weight), 4);
	EXPECT_EQ(std::get<int64_t>(score), 20);
}

TEST_F(CppApiTest, ColumnarBulkInsertMaintainsPreexistingPropertyIndexes) {
	ASSERT_TRUE(db->createNodePropertyIndexes("ColumnarIndexedUser", {"id", "score"}));

	auto txn = db->beginBulkTransaction();
	const std::vector<zyx::PropertyColumn> nodeColumns{
			{"id", {std::string("idx-c1"), std::string("idx-c2"), std::string("idx-c3")}},
			{"score", {int64_t{101}, int64_t{202}, int64_t{303}}},
	};
	const auto ids = db->createNodesColumnar("ColumnarIndexedUser", 3, nodeColumns);
	ASSERT_EQ(ids.size(), 3U);
	txn.commit();

	auto result = db->execute("MATCH (n:ColumnarIndexedUser {id: 'idx-c2'}) RETURN n.score");
	ASSERT_TRUE(result.isSuccess());
	ASSERT_TRUE(result.hasNext());
	result.next();
	auto score = result.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(score));
	EXPECT_EQ(std::get<int64_t>(score), 202);
}

TEST_F(CppApiTest, ColumnarBulkInsertValidatesShape) {
	EXPECT_THROW(
			{ (void) db->createNodesColumnar("BadColumn", 2, {zyx::PropertyColumn{"id", {std::string("one")}}}); },
			std::invalid_argument);
	EXPECT_THROW(
			{
					(void) db->createNodesColumnar(
						"BadColumn", 1,
						{zyx::PropertyColumn{"id", {std::string("one")}},
						 zyx::PropertyColumn{"id", {std::string("duplicate")}}});
			},
			std::invalid_argument);
	EXPECT_THROW(
			{ (void) db->createNodesColumnar("BadColumn", 1, {zyx::PropertyColumn{"", {int64_t{1}}}}); },
			std::invalid_argument);
	EXPECT_THROW(
			{ (void) db->createEdgesColumnar("BAD_EDGE", {1, 2}, {2}, {}); },
			std::invalid_argument);

	auto emptyIds = db->createNodesColumnar("EmptyColumnar", 2, {});
	EXPECT_EQ(emptyIds.size(), 2U);
	auto emptyEdges = db->createEdgesColumnar("EMPTY_PROPS", {emptyIds[0]}, {emptyIds[1]}, {});
	EXPECT_EQ(emptyEdges.size(), 1U);
}

TEST_F(CppApiTest, BulkTransactionCanBeFollowedByPropertyIndexBuild) {
	auto txn = db->beginBulkTransaction();
	auto ids = db->createNodes("BulkIndexedUser",
							   {{{"id", std::string("bulk-index-1")}, {"score", int64_t{11}}},
								{{"id", std::string("bulk-index-2")}, {"score", int64_t{22}}}});
	ASSERT_EQ(ids.size(), 2U);
	txn.commit();

	ASSERT_TRUE(db->createNodePropertyIndexes("BulkIndexedUser", {"id", "score"}));

	auto result = db->execute("MATCH (n:BulkIndexedUser {id: 'bulk-index-2'}) RETURN n.score");
	ASSERT_TRUE(result.isSuccess());
	ASSERT_TRUE(result.hasNext());
	result.next();
	auto score = result.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(score));
	EXPECT_EQ(std::get<int64_t>(score), 22);
}

TEST_F(CppApiTest, BulkTransactionIndexBuildIncludesPreexistingNodes) {
	(void) db->createNode("MixedBulkIndexUser", {{"id", std::string("before")}, {"score", int64_t{3}}});
	db->save();

	auto txn = db->beginBulkTransaction();
	auto ids = db->createNodes("MixedBulkIndexUser",
							   {{{"id", std::string("after")}, {"score", int64_t{5}}}});
	ASSERT_EQ(ids.size(), 1U);
	txn.commit();

	ASSERT_TRUE(db->createNodePropertyIndexes("MixedBulkIndexUser", {"id"}));

	auto before = db->execute("MATCH (n:MixedBulkIndexUser {id: 'before'}) RETURN n.score");
	ASSERT_TRUE(before.isSuccess());
	ASSERT_TRUE(before.hasNext());
	before.next();
	auto beforeScore = before.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(beforeScore));
	EXPECT_EQ(std::get<int64_t>(beforeScore), 3);

	auto after = db->execute("MATCH (n:MixedBulkIndexUser {id: 'after'}) RETURN n.score");
	ASSERT_TRUE(after.isSuccess());
	ASSERT_TRUE(after.hasNext());
	after.next();
	auto afterScore = after.get("n.score");
	ASSERT_TRUE(std::holds_alternative<int64_t>(afterScore));
	EXPECT_EQ(std::get<int64_t>(afterScore), 5);
}

TEST_F(CppApiTest, TransactionRollback) {
	db->createNode("Existing", {{"id", (int64_t) 1}});
	db->save();

	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:RollbackTest {name: 'should_disappear'})");
	EXPECT_TRUE(res.isSuccess());

	txn.rollback();
	EXPECT_FALSE(txn.isActive());
}

TEST_F(CppApiTest, TransactionExecuteWhenNotActive) {
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto res = txn.execute("MATCH (n) RETURN n");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
}

TEST_F(CppApiTest, TransactionMoveSemantics) {
	auto txn1 = db->beginTransaction();
	EXPECT_TRUE(txn1.isActive());

	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_TRUE(txn2.isActive());
	EXPECT_FALSE(txn1.isActive());

	txn2.rollback();
	EXPECT_FALSE(txn2.isActive());
}

TEST_F(CppApiTest, BeginTransactionOnClosedDb) {
	db->close();

	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_txn_test_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	auto txn = newDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.rollback();

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, TransactionExecuteAndQueryData) {
	auto txn = db->beginTransaction();

	auto createRes = txn.execute("CREATE (n:TxnData {name: 'txn_node', value: 42})");
	EXPECT_TRUE(createRes.isSuccess());

	auto queryRes = txn.execute("MATCH (n:TxnData) RETURN n.name, n.value");
	EXPECT_TRUE(queryRes.isSuccess());

	if (queryRes.hasNext()) {
		queryRes.next();
		auto name = queryRes.get("n.name");
		EXPECT_TRUE(std::holds_alternative<std::string>(name));
	}

	txn.commit();
}

TEST_F(CppApiTest, TransactionCommitOnMovedFrom) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_THROW(txn1.commit(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionRollbackOnMovedFrom) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_THROW(txn1.rollback(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, ExecuteWithExplicitTransaction) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = db->execute("RETURN 1 AS val");
	(void)res;

	txn.rollback();
}

TEST_F(CppApiTest, TransactionWithDeleteAndCommit) {
	db->createNode("TxnDel", {{"name", "alice"}});
	db->createNode("TxnDel", {{"name", "bob"}});
	db->save();

	auto txn = db->beginTransaction();
	auto res = txn.execute("MATCH (n:TxnDel {name: 'alice'}) DELETE n");
	EXPECT_TRUE(res.isSuccess());
	txn.commit();

	auto queryRes = db->execute("MATCH (n:TxnDel) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "bob");
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CppApiTest, TransactionExecuteSuccessWithDuration) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnDuration {val: 42}) RETURN n.val");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_GE(res.getDuration(), 0.0);

	if (res.hasNext()) {
		res.next();
		auto val = res.get("n.val");
		EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	}

	txn.commit();
}

TEST_F(CppApiTest, TransactionMoveAssignmentOperator) {
	auto txn1 = db->beginTransaction();
	txn1.commit();

	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());

	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_FALSE(txn2.isActive());

	txn1.rollback();
}

TEST_F(CppApiTest, TransactionExecuteOnMovedFromReturnsError) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	auto res = txn1.execute("RETURN 1");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
	EXPECT_FALSE(res.hasNext());
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionExecuteErrorPaths) {
	auto txn = db->beginTransaction();

	auto res = txn.execute("COMPLETELY INVALID !@#$%^&*()");
	EXPECT_FALSE(res.isSuccess());
	auto err = res.getError();
	EXPECT_FALSE(err.empty());

	EXPECT_FALSE(res.hasNext());
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_EQ(res.getDuration(), 0.0);

	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto valIdx = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(valIdx));

	txn.rollback();
}

TEST_F(CppApiTest, HasActiveTransactionBranch) {
	auto res1 = db->execute("RETURN 1 AS val");
	EXPECT_TRUE(res1.isSuccess());

	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res2 = db->execute("RETURN 2 AS val");
	(void)res2;

	txn.rollback();
}

TEST_F(CppApiTest, ExecuteImplicitTransactionCommit) {
	auto res = db->execute("CREATE (n:ImplTxn {val: 99}) RETURN n.val");
	ASSERT_TRUE(res.isSuccess());

	auto queryRes = db->execute("MATCH (n:ImplTxn) RETURN n.val");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto val = queryRes.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 99);
}
