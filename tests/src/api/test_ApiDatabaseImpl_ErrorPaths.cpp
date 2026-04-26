/**
 * @file test_ApiDatabaseImpl_ErrorPaths.cpp
 * @brief Branch coverage tests for DatabaseImpl.cpp error/edge-case paths.
 */

#include "ApiTestFixture.hpp"

#include "graph/core/Database.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/DataManager.hpp"

TEST_F(CppApiTest, TransactionExecuteOnInactiveTransaction) {
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto res = txn.execute("RETURN 1");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_NE(res.getError().find("not active"), std::string::npos);
}

TEST_F(CppApiTest, TransactionExecuteWithParamsOnInactiveTransaction) {
	auto txn = db->beginTransaction();
	txn.commit();

	auto res = txn.execute("RETURN $x", {{"x", int64_t(1)}});
	EXPECT_FALSE(res.isSuccess());
	EXPECT_NE(res.getError().find("not active"), std::string::npos);
}

TEST_F(CppApiTest, ResultGetOnUninitializedReturnsMonostate) {
	zyx::Result res;
	EXPECT_TRUE(std::holds_alternative<std::monostate>(res.get("any")));
	EXPECT_TRUE(std::holds_alternative<std::monostate>(res.get(0)));
}

TEST_F(CppApiTest, ResultGetColumnCountOnUninitializedReturnsZero) {
	zyx::Result res;
	EXPECT_EQ(res.getColumnCount(), 0);
}

TEST_F(CppApiTest, ResultGetColumnNameOnUninitializedReturnsEmpty) {
	zyx::Result res;
	EXPECT_EQ(res.getColumnName(0), "");
}

TEST_F(CppApiTest, ResultGetDurationOnUninitializedReturnsZero) {
	zyx::Result res;
	EXPECT_DOUBLE_EQ(res.getDuration(), 0.0);
}

TEST_F(CppApiTest, ResultIsSuccessOnUninitializedReturnsFalse) {
	zyx::Result res;
	EXPECT_FALSE(res.isSuccess());
}

TEST_F(CppApiTest, ResultGetErrorOnUninitializedReturnsMessage) {
	zyx::Result res;
	EXPECT_EQ(res.getError(), "Result not initialized");
}

TEST_F(CppApiTest, ResultHasNextOnUninitializedReturnsFalse) {
	zyx::Result res;
	EXPECT_FALSE(res.hasNext());
}

TEST_F(CppApiTest, NestedBeginTransactionThrows) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess());

	auto nestedRes = db->execute("BEGIN");
	EXPECT_FALSE(nestedRes.isSuccess());
	EXPECT_NE(nestedRes.getError().find("Nested"), std::string::npos);

	db->execute("ROLLBACK");
}

TEST_F(CppApiTest, CommitWithoutTransactionThrows) {
	auto res = db->execute("COMMIT");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_NE(res.getError().find("No active transaction"), std::string::npos);
}

TEST_F(CppApiTest, RollbackExplicitTransaction) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	auto rbRes = db->execute("ROLLBACK");
	EXPECT_TRUE(rbRes.isSuccess());
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(CppApiTest, ReadOnlyTransactionExecute) {
	db->createNode("RONode", {{"v", int64_t(1)}});
	db->save();

	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_TRUE(txn.isReadOnly());

	auto res = txn.execute("MATCH (n:RONode) RETURN n.v");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.v");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 1);

	txn.commit();
}

TEST_F(CppApiTest, ReadOnlyTransactionExecuteWithParams) {
	db->createNode("ROParam", {{"v", int64_t(10)}});
	db->save();

	auto txn = db->beginReadOnlyTransaction();
	auto res = txn.execute("MATCH (n:ROParam) WHERE n.v > $min RETURN n.v",
	                        {{"min", int64_t(5)}});
	EXPECT_TRUE(res.isSuccess());
	txn.commit();
}

TEST_F(CppApiTest, GetByIndexBeyondColumnCountReturnsMonostate) {
	db->createNode("IdxTest", {{"a", int64_t(1)}});
	auto res = db->execute("MATCH (n:IdxTest) RETURN n.a");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get(999);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto neg = res.get(-1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(neg));
}

TEST_F(CppApiTest, GetByKeyFuzzyMatchDotNotation) {
	db->createNode("FuzzyTest", {{"name", std::string("Alice")}});
	auto res = db->execute("MATCH (n:FuzzyTest) RETURN n.name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "Alice");
}

TEST_F(CppApiTest, EntityStreamModePropertyAccess) {
	db->createNode("StreamNode", {{"prop", std::string("value")}});
	auto res = db->execute("MATCH (n:StreamNode) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto propVal = res.get("prop");
	EXPECT_TRUE(std::holds_alternative<std::string>(propVal));
	EXPECT_EQ(std::get<std::string>(propVal), "value");
}

TEST_F(CppApiTest, EdgeStreamModePropertyAccess) {
	db->createNode("ES1", {{"id", int64_t(1)}});
	db->createNode("ES2", {{"id", int64_t(2)}});
	db->execute("MATCH (a:ES1), (b:ES2) CREATE (a)-[r:ESTREAM {w: 5}]->(b)");

	auto res = db->execute("MATCH ()-[r:ESTREAM]->() RETURN r");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto wVal = res.get("w");
	EXPECT_TRUE(std::holds_alternative<int64_t>(wVal));
	EXPECT_EQ(std::get<int64_t>(wVal), 5);
}

TEST_F(CppApiTest, EntityStreamMissingPropertyReturnsMonostate) {
	db->createNode("MissProp", {{"a", int64_t(1)}});
	auto res = db->execute("MATCH (n:MissProp) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("nonexistent");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, CreateNodesAndEdgesBatch) {
	auto nodeIds = db->createNodes("Batch", {
		{{"v", int64_t(1)}},
		{{"v", int64_t(2)}},
		{{"v", int64_t(3)}}
	});
	EXPECT_EQ(nodeIds.size(), 3UL);

	auto edgeIds = db->createEdges("BLINK", {
		{nodeIds[0], nodeIds[1], {{"w", int64_t(10)}}},
		{nodeIds[1], nodeIds[2], {}}
	});
	EXPECT_EQ(edgeIds.size(), 2UL);
}

TEST_F(CppApiTest, CreateNodesEmpty) {
	auto ids = db->createNodes("Empty", {});
	EXPECT_TRUE(ids.empty());
}

TEST_F(CppApiTest, CreateEdgesEmpty) {
	auto ids = db->createEdges("Empty", {});
	EXPECT_TRUE(ids.empty());
}

TEST_F(CppApiTest, MapPropertyConversionRoundTrip) {
	auto map = std::make_shared<zyx::ValueMap>();
	map->entries["a"] = int64_t(1);
	map->entries["b"] = std::string("two");

	db->createNode("MapNode", {{"data", map}});
	db->save();

	auto res = db->execute("MATCH (n:MapNode) RETURN n.data");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.data");
	EXPECT_FALSE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, ListPropertyConversionRoundTrip) {
	auto list = std::make_shared<zyx::ValueList>();
	list->elements.push_back(int64_t(10));
	list->elements.push_back(std::string("hello"));

	db->createNode("ListNode", {{"items", list}});
	db->save();

	auto res = db->execute("MATCH (n:ListNode) RETURN n.items");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.items");
	EXPECT_FALSE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, NullListPropertyConversion) {
	std::shared_ptr<zyx::ValueList> nullList;
	db->createNode("NullListNode", {{"items", nullList}});
	db->save();

	auto res = db->execute("MATCH (n:NullListNode) RETURN n");
	EXPECT_TRUE(res.isSuccess());
}

TEST_F(CppApiTest, NullMapPropertyConversion) {
	std::shared_ptr<zyx::ValueMap> nullMap;
	db->createNode("NullMapNode", {{"data", nullMap}});
	db->save();

	auto res = db->execute("MATCH (n:NullMapNode) RETURN n");
	EXPECT_TRUE(res.isSuccess());
}

TEST_F(CppApiTest, GetBeforeCursorStartReturnsMonostate) {
	db->createNode("CursorTest", {{"v", int64_t(1)}});
	auto res = db->execute("MATCH (n:CursorTest) RETURN n.v");
	EXPECT_TRUE(res.hasNext());

	auto val = res.get("n.v");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, SaveWithOpenDatabase) {
	db->createNode("SaveTest", {{"v", int64_t(1)}});
	EXPECT_NO_THROW(db->save());
}

TEST_F(CppApiTest, OpenIfExistsOnOpenDatabase) {
	EXPECT_TRUE(db->openIfExists());
}

TEST_F(CppApiTest, BeginTransactionAutoOpensClosedDb) {
	auto tempDir = fs::temp_directory_path();
	auto path = (tempDir / ("auto_open_txn_" + std::to_string(std::rand()))).string();
	auto freshDb = std::make_unique<zyx::Database>(path);
	freshDb->open();
	freshDb->close();

	auto txn = freshDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.commit();

	freshDb->close();
	std::error_code ec;
	fs::remove_all(path, ec);
	fs::remove(path + "-wal", ec);
}

TEST_F(CppApiTest, ReverseDotNotationFuzzyMatch) {
	(void)db->createNode("RevDot", {{"name", std::string("Bob")}});
	auto res = db->execute("MATCH (n:RevDot) RETURN n.name AS name");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("prefix.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "Bob");
}

TEST_F(CppApiTest, RollbackWithoutTransactionThrows) {
	auto res = db->execute("ROLLBACK");
	EXPECT_FALSE(res.isSuccess());
}

TEST_F(CppApiTest, ShortestPathBasic) {
	auto id1 = db->createNode("SP", {{"v", int64_t(1)}});
	auto id2 = db->createNode("SP", {{"v", int64_t(2)}});
	auto id3 = db->createNode("SP", {{"v", int64_t(3)}});
	db->createEdge(id1, id2, "LINK", {});
	db->createEdge(id2, id3, "LINK", {});
	db->save();

	auto path = db->getShortestPath(id1, id3, 5);
	EXPECT_GE(path.size(), 2UL);
}

TEST_F(CppApiTest, BfsTraversal) {
	auto id1 = db->createNode("BFS", {{"v", int64_t(1)}});
	auto id2 = db->createNode("BFS", {{"v", int64_t(2)}});
	db->createEdge(id1, id2, "CONN", {});
	db->save();

	std::vector<int64_t> visited;
	db->bfs(id1, [&](const zyx::Node &n) {
		visited.push_back(n.id);
		return true;
	});
	EXPECT_GE(visited.size(), 1UL);
}

TEST_F(CppApiTest, HasActiveTransactionWithCypherTxn) {
	auto beginRes = db->execute("BEGIN");
	EXPECT_TRUE(beginRes.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	auto commitRes = db->execute("COMMIT");
	EXPECT_TRUE(commitRes.isSuccess());
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(CppApiTest, GetAfterAllRowsConsumedReturnsMonostate) {
	(void)db->createNode("Consumed", {{"v", int64_t(1)}});
	auto res = db->execute("MATCH (n:Consumed) RETURN n.v");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.v");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));

	EXPECT_FALSE(res.hasNext());
	res.next();
	auto val2 = res.get("n.v");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val2));
}

TEST_F(CppApiTest, EntityStreamGetEmptyKeyReturnsEntity) {
	(void)db->createNode("EmptyKey", {{"v", int64_t(1)}});
	auto res = db->execute("MATCH (n:EmptyKey) RETURN n");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("");
	EXPECT_FALSE(std::holds_alternative<std::monostate>(val));
}

TEST_F(CppApiTest, HasActiveTransactionWithoutCypherTxn) {
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(CppApiTest, ResultMoveAssignment) {
	auto res1 = db->execute("RETURN 1");
	EXPECT_TRUE(res1.isSuccess());

	zyx::Result res2;
	res2 = std::move(res1);
	EXPECT_TRUE(res2.isSuccess());
}

TEST_F(CppApiTest, TransactionMoveAssignment) {
	auto txn1 = db->beginTransaction();
	txn1.commit();

	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());

	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	txn1.commit();
}

TEST_F(CppApiTest, BeginReadOnlyTransactionOnOpenDb) {
	// Test read-only transaction on an already-open DB (via the shared fixture)
	auto txn = db->beginReadOnlyTransaction();
	EXPECT_TRUE(txn.isActive());
	EXPECT_TRUE(txn.isReadOnly());
	txn.commit();
}
