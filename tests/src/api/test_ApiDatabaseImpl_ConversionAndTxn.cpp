/**
 * @file test_ApiDatabaseImpl_ConversionAndTxn.cpp
 * @brief Focused branch tests for DatabaseImpl conversion/txn paths.
 */

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "ApiTestFixture.hpp"
#include "graph/core/TemporalTypes.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/data/DataManager.hpp"

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

TEST_F(CppApiTest, PublicApiCollectNodeDoesNotExposeDriverAbiEntityMarkerMap) {
	db->execute("CREATE (:CppCollectNode {name:'Ada'})");

	auto res = db->execute("MATCH (n:CppCollectNode) RETURN collect(n) AS nodes");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("nodes");
	ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(val));
	const auto vec = std::get<std::vector<std::string>>(val);
	ASSERT_EQ(vec.size(), 1UL);
	EXPECT_EQ(vec[0], "1");
	EXPECT_EQ(vec[0].find("__zyx_driver_entity"), std::string::npos);
}

TEST_F(CppApiTest, PublicApiMapPropertyValueRemainsStringifiedForCompatibility) {
	auto map = std::make_shared<zyx::ValueMap>();
	map->entries["x"] = static_cast<int64_t>(7);
	db->createNode("CppMapValueNode", {{"map", map}});
	db->save();

	auto res = db->execute("MATCH (n:CppMapValueNode) RETURN n.map");
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.map");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
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

	auto schemaRes = db->execute("CREATE INDEX ON :CppApiSchemaBranch(prop)");
	EXPECT_TRUE(schemaRes.isSuccess()) << schemaRes.getError();
}

TEST_F(CppApiTest, CreateNodePropertyIndexesBuildsMultipleIndexes) {
	const auto ids = db->createNodes(
			"User",
			{{{"id", std::string("user-1")}, {"country", std::string("CN")}, {"age", int64_t{31}}},
			 {{"id", std::string("user-2")}, {"country", std::string("US")}, {"age", int64_t{42}}}});
	ASSERT_EQ(ids.size(), 2U);

	EXPECT_TRUE(db->createNodePropertyIndexes("User", {"id", "country", "age"}));

	auto showRes = db->execute("SHOW INDEXES");
	ASSERT_TRUE(showRes.isSuccess()) << showRes.getError();
	size_t userPropertyIndexes = 0;
	while (showRes.hasNext()) {
		showRes.next();
		if (std::get<std::string>(showRes.get("label")) == "User") {
			++userPropertyIndexes;
		}
	}
	EXPECT_EQ(userPropertyIndexes, 3U);

	auto idRes = db->execute("MATCH (u:User {id: 'user-1'}) RETURN count(u)");
	ASSERT_TRUE(idRes.isSuccess()) << idRes.getError();
	ASSERT_TRUE(idRes.hasNext());
	idRes.next();
	EXPECT_EQ(std::get<int64_t>(idRes.get("count(u)")), 1);

	auto countryRes = db->execute("MATCH (u:User {country: 'CN'}) RETURN count(u)");
	ASSERT_TRUE(countryRes.isSuccess()) << countryRes.getError();
	ASSERT_TRUE(countryRes.hasNext());
	countryRes.next();
	EXPECT_EQ(std::get<int64_t>(countryRes.get("count(u)")), 1);

	auto ageRes = db->execute("MATCH (u:User) WHERE u.age >= 40 RETURN count(u)");
	ASSERT_TRUE(ageRes.isSuccess()) << ageRes.getError();
	ASSERT_TRUE(ageRes.hasNext());
	ageRes.next();
	EXPECT_EQ(std::get<int64_t>(ageRes.get("count(u)")), 1);
}

TEST_F(CppApiTest, ExecuteLogsWhenSlowLogThresholdIsZero) {
	auto enableRes = db->execute("CALL dbms.setConfig('query.slow_log.enabled', true)");
	ASSERT_TRUE(enableRes.isSuccess()) << enableRes.getError();
	auto thresholdRes = db->execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)");
	ASSERT_TRUE(thresholdRes.isSuccess()) << thresholdRes.getError();

	ASSERT_TRUE(db->execute("BEGIN").isSuccess());
	ASSERT_TRUE(db->execute("COMMIT").isSuccess());
	auto res = db->execute("RETURN 1 AS one");
	EXPECT_TRUE(res.isSuccess()) << res.getError();
}

TEST_F(CppApiTest, TransactionControlTokensAcceptTrailingDelimitersAndWhitespace) {
	auto dottedToken = db->execute("BEGIN.foo");
	EXPECT_FALSE(dottedToken.isSuccess());

	auto noCommit = db->execute("COMMIT;");
	EXPECT_FALSE(noCommit.isSuccess());
	EXPECT_NE(noCommit.getError().find("No active transaction"), std::string::npos);

	auto noRollback = db->execute("  ROLLBACK \t");
	EXPECT_FALSE(noRollback.isSuccess());
	EXPECT_NE(noRollback.getError().find("No active transaction"), std::string::npos);

	auto begin = db->execute("\tBEGIN;");
	ASSERT_TRUE(begin.isSuccess()) << begin.getError();
	EXPECT_TRUE(db->hasActiveTransaction());

	auto nested = db->execute("BEGIN ");
	EXPECT_FALSE(nested.isSuccess());
	EXPECT_NE(nested.getError().find("Nested transactions"), std::string::npos);
	EXPECT_TRUE(db->hasActiveTransaction());

	auto rollback = db->execute("ROLLBACK;");
	EXPECT_TRUE(rollback.isSuccess()) << rollback.getError();
	EXPECT_FALSE(db->hasActiveTransaction());
}

TEST_F(CppApiTest, ResultGetSupportsFuzzySuffixLookupInBothDirections) {
	db->execute("CREATE (:FuzzyLookup {name:'Ada'})");

	auto propertyRes = db->execute("MATCH (n:FuzzyLookup) RETURN n.name");
	ASSERT_TRUE(propertyRes.isSuccess()) << propertyRes.getError();
	ASSERT_TRUE(propertyRes.hasNext());
	propertyRes.next();
	EXPECT_EQ(std::get<std::string>(propertyRes.get("name")), "Ada");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(propertyRes.get("person.name")));

	auto aliasRes = db->execute("RETURN 42 AS n");
	ASSERT_TRUE(aliasRes.isSuccess()) << aliasRes.getError();
	ASSERT_TRUE(aliasRes.hasNext());
	aliasRes.next();
	EXPECT_EQ(std::get<int64_t>(aliasRes.get("person.n")), 42);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(aliasRes.get("person.missing")));
}

TEST_F(CppApiTest, ResultEntityStreamExposesNodeAndEdgeAliasesPropertiesAndMissingValues) {
	int64_t adaId = db->createNode("EntityStreamPerson", {{"name", std::string("Ada")}});
	int64_t bobId = db->createNode("EntityStreamPerson", {{"name", std::string("Bob")}});
	int64_t edgeId = db->createEdge(adaId, bobId, "ENTITY_STREAM_LINK", {{"since", (int64_t) 1843}});
	ASSERT_GT(edgeId, 0);

	auto nodeRes = db->execute("MATCH (n:EntityStreamPerson {name:'Ada'}) RETURN n");
	ASSERT_TRUE(nodeRes.isSuccess()) << nodeRes.getError();
	ASSERT_TRUE(nodeRes.hasNext());
	nodeRes.next();
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeRes.get("")));
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(nodeRes.get("n")));
	EXPECT_EQ(std::get<std::string>(nodeRes.get("name")), "Ada");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nodeRes.get("missing")));

	auto edgeRes = db->execute("MATCH ()-[e:ENTITY_STREAM_LINK]->() RETURN e");
	ASSERT_TRUE(edgeRes.isSuccess()) << edgeRes.getError();
	ASSERT_TRUE(edgeRes.hasNext());
	edgeRes.next();
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeRes.get("")));
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Edge>>(edgeRes.get("e")));
	EXPECT_EQ(std::get<int64_t>(edgeRes.get("since")), 1843);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(edgeRes.get("missing")));
}

TEST_F(CppApiTest, EntityStreamPreservesEmptyLabelsForUnlabeledNodes) {
	int64_t nodeId = db->createNode(std::vector<std::string>{}, {{"name", std::string("NoLabel")}});

	auto res = db->execute("MATCH (n) RETURN n");
	ASSERT_TRUE(res.isSuccess()) << res.getError();
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto value = res.get("");
	ASSERT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(value));
	auto node = std::get<std::shared_ptr<zyx::Node>>(value);
	ASSERT_NE(node, nullptr);
	EXPECT_EQ(node->id, nodeId);
	EXPECT_TRUE(node->labels.empty());
	EXPECT_TRUE(node->label.empty());
	EXPECT_EQ(std::get<std::string>(res.get("name")), "NoLabel");
}

TEST_F(CppApiTest, DefaultAndCursorBoundaryTypedResultValuesReturnNull) {
	zyx::Result empty;
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zyx::detail::getTypedResultValue(empty, 0)));

	auto res = db->execute("RETURN 99 AS value");
	ASSERT_TRUE(res.isSuccess()) << res.getError();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zyx::detail::getTypedResultValue(res, 0)));

	ASSERT_TRUE(res.hasNext());
	res.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zyx::detail::getTypedResultValue(res, -1)));
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zyx::detail::getTypedResultValue(res, res.getColumnCount())));
	EXPECT_EQ(std::get<int64_t>(zyx::detail::getTypedResultValue(res, 0)), 99);

	res.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(zyx::detail::getTypedResultValue(res, 0)));
}

TEST_F(CppApiTest, TransactionReadOnlyFlagsCoverWriteAndAutoOpenedReadOnlyTransactions) {
	auto writeTxn = db->beginTransaction();
	ASSERT_TRUE(writeTxn.isActive());
	EXPECT_FALSE(writeTxn.isReadOnly());
	writeTxn.rollback();
	EXPECT_FALSE(writeTxn.isActive());

	auto tempDir = fs::temp_directory_path();
	auto freshPath = (tempDir / ("readonly_auto_open_test_" + std::to_string(std::rand()))).string();
	{
		zyx::Database freshDb(freshPath);
		auto readOnlyTxn = freshDb.beginReadOnlyTransaction();
		ASSERT_TRUE(readOnlyTxn.isActive());
		EXPECT_TRUE(readOnlyTxn.isReadOnly());
		readOnlyTxn.rollback();
		freshDb.close();
	}
	std::error_code ec;
	fs::remove_all(freshPath, ec);
	fs::remove(freshPath + "-wal", ec);
}

TEST_F(CppApiTest, CreateEdgesShortestPathAndBfsUseImplicitTransactions) {
	int64_t first = db->createNode("GraphApiNode", {{"name", std::string("first")}});
	int64_t middle = db->createNode("GraphApiNode", {{"name", std::string("middle")}});
	int64_t last = db->createNode("GraphApiNode", {{"name", std::string("last")}});

	auto edgeIds = db->createEdges(
		"GRAPH_API_LINK",
		{
			{first, middle, {{"order", (int64_t) 1}}},
			{middle, last, {{"order", (int64_t) 2}}},
		});
	ASSERT_EQ(edgeIds.size(), 2UL);
	EXPECT_GT(edgeIds[0], 0);
	EXPECT_GT(edgeIds[1], 0);
	db->save();

	auto edgeRes = db->execute("MATCH ()-[e:GRAPH_API_LINK]->() RETURN e.order");
	ASSERT_TRUE(edgeRes.isSuccess()) << edgeRes.getError();
	auto traversalRes = db->execute("MATCH (:GraphApiNode {name: 'first'})-[:GRAPH_API_LINK]->(n) RETURN count(n)");
	ASSERT_TRUE(traversalRes.isSuccess()) << traversalRes.getError();
	ASSERT_TRUE(traversalRes.hasNext());
	traversalRes.next();
	const auto traversalCount = traversalRes.get("count(n)");
	ASSERT_TRUE(std::holds_alternative<int64_t>(traversalCount));
	EXPECT_EQ(std::get<int64_t>(traversalCount), 1);

	std::vector<zyx::Node> path;
	EXPECT_NO_THROW(path = db->getShortestPath(first, last, 4));
	EXPECT_EQ(path.size(), 3UL);

	std::vector<int64_t> visited;
	EXPECT_NO_THROW(db->bfs(first, [&](const zyx::Node &node) {
		visited.push_back(node.id);
		return visited.size() < 3;
	}));
	EXPECT_NE(std::find(visited.begin(), visited.end(), first), visited.end());
	EXPECT_NE(std::find(visited.begin(), visited.end(), last), visited.end());
}

// ============================================================================
// toPublicValue for temporal types (TemporalDate, TemporalDateTime, TemporalDuration)
// These are stored via the internal DataManager API and read back through the
// C++ public API (Result::get()), hitting the temporal branches of toPublicValue.
// ============================================================================

TEST_F(CppApiTest, TemporalDate_ConversionToPublicValue) {
	// Store a TemporalDate via Cypher date() function if supported,
	// or use internal graph::Database directly.
	// We create a standalone test using graph::Database to bypass the zyx wrapper.
	auto tempDir = fs::temp_directory_path();
	auto path = (tempDir / ("temporal_date_test_" + std::to_string(std::rand()))).string();
	{
		graph::Database internalDb(path);
		internalDb.open();

		auto dm = internalDb.getStorage()->getDataManager();
		int64_t labelId = dm->getOrCreateTokenId("TemporalNode");
		graph::Node node(0, labelId);
		auto txn = internalDb.beginTransaction();
		dm->addNode(node);
		int64_t nodeId = node.getId();

		graph::TemporalDate date = graph::TemporalDate::fromISO("2024-06-15");
		dm->addNodeProperties(nodeId, {{"birthday", graph::PropertyValue(date)}});
		txn.commit();
		internalDb.close();
	}

	// Now open via public zyx API and read back — hits TemporalDate branch
	zyx::Database zyxDb(path);
	zyxDb.open();

	auto res = zyxDb.execute("MATCH (n:TemporalNode) RETURN n.birthday");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.birthday");
	// TemporalDate converts to ISO string via toPublicValue
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	const auto &s = std::get<std::string>(val);
	EXPECT_NE(s.find("2024"), std::string::npos) << "ISO date should contain year 2024: " << s;

	zyxDb.close();
	std::error_code ec;
	fs::remove_all(path, ec);
	fs::remove(path + "-wal", ec);
}

TEST_F(CppApiTest, TemporalDateTime_ConversionToPublicValue) {
	auto tempDir = fs::temp_directory_path();
	auto path = (tempDir / ("temporal_dt_test_" + std::to_string(std::rand()))).string();
	{
		graph::Database internalDb(path);
		internalDb.open();

		auto dm = internalDb.getStorage()->getDataManager();
		int64_t labelId = dm->getOrCreateTokenId("EventNode");
		graph::Node node(0, labelId);
		auto txn = internalDb.beginTransaction();
		dm->addNode(node);
		int64_t nodeId = node.getId();

		graph::TemporalDateTime dt = graph::TemporalDateTime::fromISO("2024-06-15T10:30:00");
		dm->addNodeProperties(nodeId, {{"timestamp", graph::PropertyValue(dt)}});
		txn.commit();
		internalDb.close();
	}

	zyx::Database zyxDb(path);
	zyxDb.open();

	auto res = zyxDb.execute("MATCH (n:EventNode) RETURN n.timestamp");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.timestamp");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	const auto &s = std::get<std::string>(val);
	EXPECT_NE(s.find("2024"), std::string::npos) << "ISO datetime should contain year 2024: " << s;

	zyxDb.close();
	std::error_code ec;
	fs::remove_all(path, ec);
	fs::remove(path + "-wal", ec);
}

TEST_F(CppApiTest, TemporalDuration_ConversionToPublicValue) {
	auto tempDir = fs::temp_directory_path();
	auto path = (tempDir / ("temporal_dur_test_" + std::to_string(std::rand()))).string();
	{
		graph::Database internalDb(path);
		internalDb.open();

		auto dm = internalDb.getStorage()->getDataManager();
		int64_t labelId = dm->getOrCreateTokenId("DurationNode");
		graph::Node node(0, labelId);
		auto txn = internalDb.beginTransaction();
		dm->addNode(node);
		int64_t nodeId = node.getId();

		graph::TemporalDuration dur = graph::TemporalDuration::fromComponents(1, 2, 0, 3, 4, 5, 6);
		dm->addNodeProperties(nodeId, {{"tenure", graph::PropertyValue(dur)}});
		txn.commit();
		internalDb.close();
	}

	zyx::Database zyxDb(path);
	zyxDb.open();

	auto res = zyxDb.execute("MATCH (n:DurationNode) RETURN n.tenure");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();

	auto val = res.get("n.tenure");
	ASSERT_TRUE(std::holds_alternative<std::string>(val));
	const auto &s = std::get<std::string>(val);
	// ISO duration starts with P
	EXPECT_EQ(s.front(), 'P') << "ISO duration should start with 'P': " << s;

	zyxDb.close();
	std::error_code ec;
	fs::remove_all(path, ec);
	fs::remove(path + "-wal", ec);
}
