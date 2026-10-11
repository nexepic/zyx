/**
 * @file test_ApiDatabaseImpl_ErrorPaths.cpp
 * @brief Branch coverage tests for DatabaseImpl.cpp error/edge-case paths.
 */

#include "ApiTestFixture.hpp"

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

TEST_F(CppApiTest, CypherTransactionFailureDoesNotLeakPartialStatementWrites) {
	auto begin = db->execute("BEGIN");
	ASSERT_TRUE(begin.isSuccess()) << begin.getError();

	auto stable = db->execute("CREATE (:CypherTxnAtomic {name: 'stable'})");
	ASSERT_TRUE(stable.isSuccess()) << stable.getError();
	auto failed = db->execute(
			"FOREACH (value IN [1, 0] | CREATE (:CypherTxnAtomic {name: 'partial', quotient: 1 / value}))");
	EXPECT_FALSE(failed.isSuccess());
	EXPECT_TRUE(db->hasActiveTransaction());

	auto staged = db->execute("MATCH (n:CypherTxnAtomic) RETURN n.name ORDER BY n.name");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	ASSERT_TRUE(staged.hasNext());
	staged.next();
	ASSERT_TRUE(std::holds_alternative<std::string>(staged.get("n.name")));
	EXPECT_EQ(std::get<std::string>(staged.get("n.name")), "stable");
	EXPECT_FALSE(staged.hasNext());

	auto commit = db->execute("COMMIT");
	ASSERT_TRUE(commit.isSuccess()) << commit.getError();
	EXPECT_FALSE(db->hasActiveTransaction());

	auto committed = db->execute("MATCH (n:CypherTxnAtomic) RETURN n.name ORDER BY n.name");
	ASSERT_TRUE(committed.isSuccess()) << committed.getError();
	ASSERT_TRUE(committed.hasNext());
	committed.next();
	ASSERT_TRUE(std::holds_alternative<std::string>(committed.get("n.name")));
	EXPECT_EQ(std::get<std::string>(committed.get("n.name")), "stable");
	EXPECT_FALSE(committed.hasNext());
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

TEST_F(CppApiTest, CreateEdgesColumnarRejectsMismatchedEndpointsAndAcceptsAnEmptyBatch) {
	EXPECT_THROW(static_cast<void>(db->createEdgesColumnar("REL", {1}, {}, {})), std::invalid_argument);
	EXPECT_TRUE(db->createEdgesColumnar("REL", {}, {}, {}).empty());
}

TEST_F(CppApiTest, DirectAndColumnarEdgeWritesValidateEndpointsBeforePublication) {
	const int64_t source = db->createNode("EndpointValidation", {{"name", std::string("source")}});
	const int64_t target = db->createNode("EndpointValidation", {{"name", std::string("target")}});

	EXPECT_THROW(static_cast<void>(db->createEdge(0, target, "INVALID_ENDPOINT")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(db->createEdge(source, -1, "INVALID_ENDPOINT")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(db->createEdges(
				"INVALID_ENDPOINT", {{source, 0, {{"position", int64_t{1}}}}})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(db->createEdgesColumnar("INVALID_ENDPOINT", {source}, {0}, {})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(db->createEdgesColumnar("INVALID_ENDPOINT", {source}, {target},
				{{"", {int64_t{1}}}})),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(db->createEdgesColumnar("INVALID_ENDPOINT", {source}, {target},
				{{"weight", {int64_t{1}}}, {"weight", {int64_t{2}}}})),
			 std::invalid_argument);

	const zyx::Result invalidRows = db->execute("MATCH ()-[edge:INVALID_ENDPOINT]->() RETURN edge");
	ASSERT_TRUE(invalidRows.isSuccess()) << invalidRows.getError();
	EXPECT_FALSE(invalidRows.hasNext());
}

TEST_F(CppApiTest, ColumnarEdgeWritesRejectInactiveOrUnknownEndpointsWithoutPartialPublication) {
	const int64_t source = db->createNode("ColumnarEndpoint", {{"name", std::string("source")}});
	const int64_t target = db->createNode("ColumnarEndpoint", {{"name", std::string("target")}});
	const int64_t removed = db->createNode("ColumnarEndpoint", {{"name", std::string("removed")}});
	const zyx::Result deleted = db->execute("MATCH (node:ColumnarEndpoint {name: 'removed'}) DELETE node");
	ASSERT_TRUE(deleted.isSuccess()) << deleted.getError();

	try {
		static_cast<void>(db->createEdgesColumnar(
				"COLUMNAR_ENDPOINT", {source, removed}, {target, source}, {{"weight", {int64_t{1}, int64_t{2}}}}));
		FAIL() << "expected the inactive endpoint to reject the complete batch";
	} catch (const zyx::BulkInputError &error) {
		EXPECT_EQ(error.rowIndex(), 1U);
		EXPECT_EQ(error.fieldPath(), "source_id");
		EXPECT_NE(std::string(error.what()).find("does not exist or is inactive"), std::string::npos);
	}

	EXPECT_THROW(static_cast<void>(db->createEdgesColumnar(
				"COLUMNAR_ENDPOINT", {source}, {target + 10'000}, {{"weight", {int64_t{3}}}})),
			 zyx::BulkInputError);

	const zyx::Result rows = db->execute("MATCH ()-[edge:COLUMNAR_ENDPOINT]->() RETURN edge");
	ASSERT_TRUE(rows.isSuccess()) << rows.getError();
	EXPECT_FALSE(rows.hasNext());
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
	auto path = (tempDir / ("auto_open_txn_" + std::to_string(
							 std::chrono::steady_clock::now().time_since_epoch().count()))).string();
	std::error_code ec;
	fs::remove(path, ec);
	auto freshDb = std::make_unique<zyx::Database>(path);
	freshDb->open();
	freshDb->close();

	auto txn = freshDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.commit();

	freshDb->close();
	fs::remove(path, ec);
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

TEST_F(CppApiTest, BfsTraversal_ErrorPaths) {
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

TEST_F(CppApiTest, ResultMoveAssignment_ErrorPaths) {
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

TEST_F(CppApiTest, BatchInputValidationHandlesSparseAndEmptyPropertyShapes) {
	EXPECT_TRUE(db->createNodesColumnar("ZeroColumnarNode", 0, {}).empty());
	EXPECT_TRUE(db->createEdgesColumnar("ZeroColumnarEdge", {}, {}, {}).empty());

	EXPECT_THROW(db->createNodes("FallbackEmptyKeyNode", {
			{{"", int64_t{1}}, {"name", std::string("first")}},
			{{"name", std::string("second")}},
	}), std::invalid_argument);

	auto mismatchedKeyNodeIds = db->createNodes("FallbackMismatchedKeyNode", {
			{{"a", int64_t{1}}, {"b", int64_t{2}}},
			{{"a", int64_t{3}}, {"c", int64_t{4}}},
	});
	ASSERT_EQ(mismatchedKeyNodeIds.size(), 2U);

	auto endpoints = db->createNodes("FallbackEdgeEndpoint", {{{"id", int64_t{1}}}, {{"id", int64_t{2}}}});
	ASSERT_EQ(endpoints.size(), 2U);

	EXPECT_THROW(db->createEdges("FALLBACK_EMPTY_KEY_EDGE", {
			{endpoints[0], endpoints[1], {{"", int64_t{1}}, {"kind", std::string("empty-key")}}},
			{endpoints[1], endpoints[0], {{"kind", std::string("second")}}},
	}), std::invalid_argument);

	auto mismatchedKeyEdgeIds = db->createEdges("FALLBACK_MISMATCHED_KEY_EDGE", {
			{endpoints[0], endpoints[1], {{"a", int64_t{1}}, {"b", int64_t{2}}}},
			{endpoints[1], endpoints[0], {{"a", int64_t{3}}, {"c", int64_t{4}}}},
	});
	ASSERT_EQ(mismatchedKeyEdgeIds.size(), 2U);
}

TEST_F(CppApiTest, NodePropertyIndexValidationAndTransactionGuards) {
	EXPECT_THROW((void)db->createNodePropertyIndexes("", {"id"}), std::invalid_argument);
	EXPECT_TRUE(db->createNodePropertyIndexes("NoRequestedProperties", {}));
	EXPECT_THROW((void)db->createNodePropertyIndexes("BadProperty", {""}), std::invalid_argument);

	auto txn = db->beginTransaction();
	ASSERT_TRUE(txn.isActive());
	EXPECT_TRUE(db->hasActiveTransaction());
	EXPECT_THROW((void)db->createNodePropertyIndexes("TxnGuarded", {"id"}), std::runtime_error);
	txn.rollback();

	auto begin = db->execute("BEGIN");
	ASSERT_TRUE(begin.isSuccess()) << begin.getError();
	EXPECT_TRUE(db->hasActiveTransaction());
	EXPECT_THROW((void)db->createNodePropertyIndexes("CypherTxnGuarded", {"id"}), std::runtime_error);
	auto rollback = db->execute("ROLLBACK");
	EXPECT_TRUE(rollback.isSuccess()) << rollback.getError();
}

TEST_F(CppApiTest, ExplicitTransactionsOwnBatchAndAlgorithmOperations) {
	auto txn = db->beginTransaction();
	ASSERT_TRUE(txn.isActive());
	EXPECT_FALSE(txn.isReadOnly());

	auto ids = txn.createNodes("ExplicitTxnBatchNode", {
			{{"name", std::string("a")}, {"rank", int64_t{1}}},
			{{"name", std::string("b")}, {"score", int64_t{2}}},
			{{"name", std::string("c")}, {"rank", int64_t{3}}},
	});
	ASSERT_EQ(ids.size(), 3U);

	auto edgeIds = txn.createEdges("EXPLICIT_TXN_LINK", {
			{ids[0], ids[1], {{"order", int64_t{1}}}},
			{ids[1], ids[2], {{"order", int64_t{2}}}},
	});
	ASSERT_EQ(edgeIds.size(), 2U);

	std::vector<zyx::Node> path;
	EXPECT_NO_THROW(path = txn.getShortestPath(ids[0], ids[2], 4));
	EXPECT_GE(path.size(), 2U);

	std::vector<int64_t> visited;
	EXPECT_NO_THROW(txn.bfs(ids[0], [&](const zyx::Node &node) {
		visited.push_back(node.id);
		return visited.size() < 4;
	}));
	EXPECT_FALSE(visited.empty());

	txn.commit();
}

TEST_F(CppApiTest, FailedMutationDoesNotLeakPartialWritesFromAnExplicitTransaction) {
	auto txn = db->beginTransaction();
	ASSERT_TRUE(txn.isActive());

	auto stable = txn.execute("CREATE (:ApiTxnAtomic {name: 'stable'})");
	ASSERT_TRUE(stable.isSuccess()) << stable.getError();
	auto failed = txn.execute(
			"FOREACH (value IN [1, 0] | CREATE (:ApiTxnAtomic {name: 'partial', quotient: 1 / value}))");
	EXPECT_FALSE(failed.isSuccess());
	EXPECT_FALSE(txn.isReadOnly());
	EXPECT_TRUE(txn.isActive());

	auto staged = txn.execute("MATCH (n:ApiTxnAtomic) RETURN n.name ORDER BY n.name");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	ASSERT_TRUE(staged.hasNext());
	staged.next();
	ASSERT_TRUE(std::holds_alternative<std::string>(staged.get("n.name")));
	EXPECT_EQ(std::get<std::string>(staged.get("n.name")), "stable");
	EXPECT_FALSE(staged.hasNext());

	txn.commit();
	auto committed = db->execute("MATCH (n:ApiTxnAtomic) RETURN n.name ORDER BY n.name");
	ASSERT_TRUE(committed.isSuccess()) << committed.getError();
	ASSERT_TRUE(committed.hasNext());
	committed.next();
	ASSERT_TRUE(std::holds_alternative<std::string>(committed.get("n.name")));
	EXPECT_EQ(std::get<std::string>(committed.get("n.name")), "stable");
	EXPECT_FALSE(committed.hasNext());
}

TEST_F(CppApiTest, BulkTransactionAutoOpensAndDefaultTransactionFlagsAreSafe) {
	auto movableTxn = db->beginTransaction();
	zyx::Transaction movedTxn = std::move(movableTxn);
	EXPECT_FALSE(movableTxn.isReadOnly());
	movedTxn.rollback();

	auto tempDir = fs::temp_directory_path();
	const std::string path = (tempDir / ("bulk_auto_open_" + std::to_string(
														  std::chrono::steady_clock::now().time_since_epoch().count()))).string();
	std::error_code ec;
	fs::remove(path, ec);
	{
		zyx::Database freshDb(path);
		auto txn = freshDb.beginBulkTransaction();
		EXPECT_TRUE(txn.isActive());
		EXPECT_TRUE(freshDb.hasActiveTransaction());
		txn.commit();
		freshDb.close();
	}

	fs::remove(path, ec);
}
