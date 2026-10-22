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

#include <chrono>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

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

TEST_F(CppApiTest, BulkTransactionSupportsTransactionBoundBatchWrites) {
	auto txn = db->beginBulkTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_FALSE(txn.isReadOnly());

	auto ids = txn.createNodes("BulkApiUser",
							   {{{"id", std::string("u1")}, {"score", int64_t{1}}},
								{{"id", std::string("u2")}, {"score", int64_t{2}}}});
	ASSERT_EQ(ids.size(), 2U);
	auto edgeIds = txn.createEdges("FOLLOWS", {{ids[0], ids[1], {{"weight", int64_t{7}}}}});
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
	const auto ids = txn.createNodesColumnar("ColumnarUser", 3, nodeColumns);
	ASSERT_EQ(ids.size(), 3U);

	const std::vector<zyx::PropertyColumn> edgeColumns{
			{"weight", {int64_t{4}, int64_t{8}}},
	};
	const auto edgeIds = txn.createEdgesColumnar(
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

TEST_F(CppApiTest, ColumnarBulkEdgesMatchOnlineEdgeTopologyPropertiesAndReopen) {
	const std::vector<int64_t> nodes = db->createNodesColumnar(
			"BulkOnlineEquivalenceNode", 3U,
			{{"name", {std::string("source"), std::string("target"), std::string("other")}}});
	ASSERT_EQ(nodes.size(), 3U);

	const std::vector<int64_t> sources = {nodes[0], nodes[0], nodes[1], nodes[2]};
	const std::vector<int64_t> targets = {nodes[1], nodes[1], nodes[1], nodes[2]};
	const std::vector<zyx::PropertyColumn> columns = {{"rank", {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{4}}}};
	const std::vector<int64_t> onlineIds = db->createEdges(
			"ONLINE_EQUIVALENCE",
			{{sources[0], targets[0], {{"rank", int64_t{1}}}}, {sources[1], targets[1], {{"rank", int64_t{2}}}},
			 {sources[2], targets[2], {{"rank", int64_t{3}}}}, {sources[3], targets[3], {{"rank", int64_t{4}}}}});
	ASSERT_EQ(onlineIds.size(), sources.size());

	auto bulk = db->beginBulkTransaction();
	const std::vector<int64_t> bulkIds = bulk.createEdgesColumnar("BULK_EQUIVALENCE", sources, targets, columns);
	ASSERT_EQ(bulkIds.size(), sources.size());
	bulk.commit();

	const auto readRows = [this](const std::string &edgeType) {
		const zyx::Result result = db->execute("MATCH (source)-[edge:" + edgeType + "]->(target) "
												"RETURN source.name, target.name, edge.rank ORDER BY edge.rank");
		EXPECT_TRUE(result.isSuccess()) << result.getError();
		std::vector<std::tuple<std::string, std::string, int64_t>> rows;
		while (result.hasNext()) {
			result.next();
			rows.emplace_back(std::get<std::string>(result.get("source.name")),
							  std::get<std::string>(result.get("target.name")),
							  std::get<int64_t>(result.get("edge.rank")));
		}
		return rows;
	};

	const auto expected = std::vector<std::tuple<std::string, std::string, int64_t>>{
			{"source", "target", 1}, {"source", "target", 2}, {"target", "target", 3}, {"other", "other", 4}};
	EXPECT_EQ(readRows("ONLINE_EQUIVALENCE"), expected);
	EXPECT_EQ(readRows("BULK_EQUIVALENCE"), expected);

	db->save();
	db->close();
	db->open();
	EXPECT_EQ(readRows("ONLINE_EQUIVALENCE"), expected);
	EXPECT_EQ(readRows("BULK_EQUIVALENCE"), expected);
}

TEST_F(CppApiTest, ColumnarBulkInsertMaintainsPreexistingPropertyIndexes) {
	ASSERT_TRUE(db->createNodePropertyIndexes("ColumnarIndexedUser", {"id", "score"}));

	auto txn = db->beginBulkTransaction();
	const std::vector<zyx::PropertyColumn> nodeColumns{
			{"id", {std::string("idx-c1"), std::string("idx-c2"), std::string("idx-c3")}},
			{"score", {int64_t{101}, int64_t{202}, int64_t{303}}},
	};
	const auto ids = txn.createNodesColumnar("ColumnarIndexedUser", 3, nodeColumns);
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

TEST_F(CppApiTest, TransactionColumnarBulkInsertValidatesEveryPropertyColumnShape) {
	auto txn = db->beginBulkTransaction();

	EXPECT_THROW(static_cast<void>(txn.createNodesColumnar(
			"BadTransactionColumn", 2U, {zyx::PropertyColumn{"value", {int64_t{1}}}})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(txn.createNodesColumnar(
			"BadTransactionColumn", 1U, {zyx::PropertyColumn{"", {int64_t{1}}}})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(txn.createNodesColumnar(
			"BadTransactionColumn", 1U,
			{zyx::PropertyColumn{"value", {int64_t{1}}}, zyx::PropertyColumn{"value", {int64_t{2}}}})),
			std::invalid_argument);

	// The failed requests must not leave a partially staged batch behind. A
	// valid row after the rejects proves the transaction remains usable.
	const auto ids = txn.createNodesColumnar("TransactionColumn", 1U,
			{zyx::PropertyColumn{"value", {int64_t{7}}}});
	ASSERT_EQ(ids.size(), 1U);
	txn.commit();

	auto result = db->execute("MATCH (n:TransactionColumn) RETURN n.value");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	ASSERT_EQ(std::get<int64_t>(result.get("n.value")), 7);
}

TEST_F(CppApiTest, TransactionColumnarEdgeWritesValidatePropertyColumnsBeforeEndpoints) {
	auto txn = db->beginTransaction();
	const auto ids = txn.createNodes("TransactionColumnarShape", {{{"v", int64_t{1}}}, {{"v", int64_t{2}}}});
	ASSERT_EQ(ids.size(), 2U);

	EXPECT_THROW(static_cast<void>(txn.createEdgesColumnar(
			"BadTransactionEdgeColumn", {ids[0]}, {ids[1]}, {zyx::PropertyColumn{"", {int64_t{1}}}})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(txn.createEdgesColumnar(
			"BadTransactionEdgeColumn", {ids[0]}, {ids[1]}, {zyx::PropertyColumn{"v", {}}})),
			std::invalid_argument);
	EXPECT_THROW(static_cast<void>(txn.createEdgesColumnar(
			"BadTransactionEdgeColumn", {ids[0]}, {ids[1]},
			{zyx::PropertyColumn{"v", {int64_t{1}}}, zyx::PropertyColumn{"v", {int64_t{2}}}})),
			std::invalid_argument);

	const auto edgeIds = txn.createEdgesColumnar("TransactionEdgeColumn", {ids[0]}, {ids[1]},
			{zyx::PropertyColumn{"weight", {int64_t{3}}}});
	ASSERT_EQ(edgeIds.size(), 1U);
	txn.commit();

	auto result = db->execute("MATCH ()-[e:TransactionEdgeColumn]->() RETURN e.weight");
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	ASSERT_TRUE(result.hasNext());
	result.next();
	ASSERT_EQ(std::get<int64_t>(result.get("e.weight")), 3);
}

TEST_F(CppApiTest, TransactionColumnarEdgesReportStructuredEndpointErrorsWithoutStagingRows) {
	auto txn = db->beginTransaction();
	const std::vector<int64_t> ids = txn.createNodes("TransactionColumnarEndpoint", {
			{{"name", std::string("source")}},
			{{"name", std::string("target")}},
	});
	ASSERT_EQ(ids.size(), 2U);

	try {
		static_cast<void>(txn.createEdgesColumnar(
				"TRANSACTION_COLUMNAR_ENDPOINT", {ids[0], 999'999}, {ids[1], ids[0]},
				{{"weight", {int64_t{1}, int64_t{2}}}}));
		FAIL() << "expected the unknown endpoint to reject the complete transaction-local batch";
	} catch (const zyx::BulkInputError &error) {
		EXPECT_EQ(error.rowIndex(), 1U);
		EXPECT_EQ(error.fieldPath(), "source_id");
	}

	const zyx::Result staged = txn.execute("MATCH ()-[edge:TRANSACTION_COLUMNAR_ENDPOINT]->() RETURN edge");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	EXPECT_FALSE(staged.hasNext());
	txn.rollback();
}

TEST_F(CppApiTest, BulkTransactionCanBeFollowedByPropertyIndexBuild) {
	auto txn = db->beginBulkTransaction();
	auto ids = txn.createNodes("BulkIndexedUser",
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
	auto ids = txn.createNodes("MixedBulkIndexUser",
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
	const std::string newPath =
			(tempDir / ("api_txn_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
						"_" + std::to_string(std::rand())))
					.string();
	const auto removeArtifacts = [&newPath] {
		std::error_code error;
		fs::remove_all(newPath, error);
	};
	// Remove the single database file before and after the test so a repeated run cannot collide with
	// a prior invocation that used the same process-global rand() sequence.
	removeArtifacts();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	auto txn = newDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.rollback();

	newDb->close();
	newDb.reset();
	removeArtifacts();
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

TEST_F(CppApiTest, MovedFromTransactionRejectsDirectGraphOperationsWithoutWriting) {
	auto active = db->beginTransaction();
	zyx::Transaction unavailable = std::move(active);

	EXPECT_FALSE(active.isActive());
	EXPECT_FALSE(active.isReadOnly());
	const auto parameterized = active.execute("RETURN $value", {{"value", int64_t{1}}});
	EXPECT_FALSE(parameterized.isSuccess());
	EXPECT_EQ(parameterized.getError(), "Transaction is not active");
	const zyx::PreparedQuery prepared = db->query().unwind({int64_t{1}}, "value").returning({"value"}).build();
	const auto preparedResult = active.execute(prepared);
	EXPECT_FALSE(preparedResult.isSuccess());
	EXPECT_EQ(preparedResult.getError(), "Transaction is not active");
	EXPECT_THROW(static_cast<void>(active.createNode("UnavailableTransaction", {})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createNode(std::vector<std::string>{"UnavailableTransaction"}, {})),
			 std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createNodes(
				"UnavailableTransaction", {{{"value", int64_t{1}}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createNodesColumnar(
				"UnavailableTransaction", 1U, {zyx::PropertyColumn{"value", {int64_t{1}}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createEdge(1, 2, "UNAVAILABLE", {})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createEdges(
				"UNAVAILABLE", {{1, 2, {{"value", int64_t{1}}}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.createEdgesColumnar(
				"UNAVAILABLE", {1}, {2}, {zyx::PropertyColumn{"value", {int64_t{1}}}})), std::logic_error);
	EXPECT_THROW(static_cast<void>(active.getShortestPath(1, 2)), std::logic_error);
	EXPECT_THROW(active.bfs(1, [](const zyx::Node &) { return true; }), std::logic_error);

	// Each rejected direct operation must leave the database unchanged.
	auto count = db->execute("MATCH (n) RETURN count(n) AS count");
	ASSERT_TRUE(count.isSuccess()) << count.getError();
	ASSERT_TRUE(count.hasNext());
	count.next();
	ASSERT_EQ(std::get<int64_t>(count.get("count")), 0);

	unavailable.rollback();
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

TEST_F(CppApiTest, BoundedImportBatchPublishesNodesEdgesAndIndexesThroughOneGraphCommit) {
    ASSERT_TRUE(db->createNodePropertyIndexes("ImportUser", {"id", "score"}));
    std::vector<zyx::PropertyColumn> columns{{"id", {}}, {"score", {}}, {"country", {}}};
    for (int64_t row = 0; row < 1000; ++row) {
        columns[0].values.push_back(std::string("import-") + std::to_string(row));
        columns[1].values.push_back(row); columns[2].values.push_back(std::string(row % 2 ? "US" : "CN"));
    }
    struct Trace {
        bool previous = graph::debug::PerfTrace::areValuesEnabled();
        Trace() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true); }
        ~Trace() { graph::debug::PerfTrace::setValueCollectionEnabled(previous); }
    } trace;
    auto transaction = db->beginBulkTransaction();
    const auto nodes = transaction.createNodesColumnar("ImportUser", 1000, columns); ASSERT_EQ(nodes.size(), 1000U);
    const auto edges = transaction.createEdgesColumnar("IMPORT_EDGE", std::vector<int64_t>(1000, nodes.front()),
        std::vector<int64_t>(1000, nodes.back()), {{"weight", std::vector<zyx::Value>(1000, int64_t{1})}});
    ASSERT_EQ(edges.size(), 1000U); transaction.commit();
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    ASSERT_TRUE(counters.contains("native_graph.write.id_reservation_calls"));
    EXPECT_EQ(counters.at("native_graph.write.id_reservation_calls").totalValue, 2);
    RecordProperty("imported_nodes", static_cast<int>(nodes.size()));
    RecordProperty("imported_edges", static_cast<int>(edges.size()));
    RecordProperty("id_range_reservations", static_cast<int>(counters.at("native_graph.write.id_reservation_calls").totalValue));
    RecordProperty("full_metadata_traversals", 0);
    EXPECT_FALSE(counters.contains("native_metadata.external_source_refresh"));
    EXPECT_FALSE(counters.contains("native_metadata.reclaim.full_traversals"));
    db->close(); db->open();
    auto node = db->execute("MATCH (u:ImportUser {id:'import-517'}) RETURN u.score AS score");
    ASSERT_TRUE(node.isSuccess()); ASSERT_TRUE(node.hasNext()); node.next(); EXPECT_EQ(std::get<int64_t>(node.get("score")), 517);
    auto counts = db->execute("MATCH (u:ImportUser) RETURN u.country AS country,count(*) AS rows");
    ASSERT_TRUE(counts.isSuccess()); size_t groups = 0;
    while (counts.hasNext()) { counts.next(); EXPECT_EQ(std::get<int64_t>(counts.get("rows")), 500); ++groups; }
    EXPECT_EQ(groups, 2U);
    auto topology = db->execute("MATCH (:ImportUser {id:'import-0'})-[r:IMPORT_EDGE]->(:ImportUser {id:'import-999'}) RETURN count(r) AS rows,sum(r.weight) AS weight");
    ASSERT_TRUE(topology.isSuccess()); ASSERT_TRUE(topology.hasNext()); topology.next();
    EXPECT_EQ(std::get<int64_t>(topology.get("rows")), 1000); EXPECT_EQ(std::get<int64_t>(topology.get("weight")), 1000);
}
