/**
 * @file test_RelocatableStorageBackend.cpp
 * @brief Integration tests for the format-safe relocatable logical read backend.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, CypherWriteOnlyCreateUsesTheLogicalMutationBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const zyx::Result create = database.execute(
			"CREATE (a:CypherOnline {name: 'Ada'})-[r:CONNECTS {since: 2026}]->(b:CypherOnline {name: 'Bob'}) "
			"RETURN a, r, b");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_EQ(create.getColumnCount(), 3);
	ASSERT_TRUE(create.hasNext());
	create.next();
	const auto createdSource = std::get<std::shared_ptr<zyx::Node>>(create.get("a"));
	const auto createdEdge = std::get<std::shared_ptr<zyx::Edge>>(create.get("r"));
	const auto createdTarget = std::get<std::shared_ptr<zyx::Node>>(create.get("b"));
	ASSERT_NE(createdSource, nullptr);
	ASSERT_NE(createdEdge, nullptr);
	ASSERT_NE(createdTarget, nullptr);
	EXPECT_EQ(std::get<std::string>(createdSource->properties.at("name")), "Ada");
	EXPECT_EQ(std::get<int64_t>(createdEdge->properties.at("since")), 2026);
	EXPECT_EQ(std::get<std::string>(createdTarget->properties.at("name")), "Bob");

	const zyx::Result parameterized = database.execute("CREATE (n:CypherParam {score: $score}) RETURN n.score AS score",
													   {{"score", int64_t(17)}});
	ASSERT_TRUE(parameterized.isSuccess()) << parameterized.getError();
	ASSERT_TRUE(parameterized.hasNext());
	parameterized.next();
	EXPECT_EQ(std::get<int64_t>(parameterized.get("score")), 17);

	const zyx::Result query = database.execute("MATCH (a:CypherOnline)-[r:CONNECTS]->(b:CypherOnline) RETURN a.name AS "
											   "source, r.since AS since, b.name AS target");
	ASSERT_TRUE(query.isSuccess()) << query.getError();
	ASSERT_TRUE(query.hasNext());
	query.next();
	EXPECT_EQ(std::get<std::string>(query.get("source")), "Ada");
	EXPECT_EQ(std::get<int64_t>(query.get("since")), 2026);
	EXPECT_EQ(std::get<std::string>(query.get("target")), "Bob");
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherCreateSetRemoveFoldsCreatedEdgeStateIntoOneNativePublication) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result create = database.execute(
			"CREATE (a:CreatedEdgeFold {seed: 3})-"
			"[r:CREATED_EDGE_FOLD {temporary: true}]->(b:CreatedEdgeFold) "
			"SET r.weight = a.seed * 2 REMOVE r.temporary "
			"RETURN r.weight AS weight, r.temporary AS temporary");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_TRUE(create.hasNext());
	create.next();
	EXPECT_EQ(std::get<int64_t>(create.get("weight")), 6);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(create.get("temporary")));
	EXPECT_FALSE(create.hasNext());
	database.close();

	zyx::Database reopened(data_path_.string());
	reopened.open();
	zyx::Result visible = reopened.execute(
			"MATCH (:CreatedEdgeFold)-[r:CREATED_EDGE_FOLD]->(:CreatedEdgeFold) "
			"RETURN r.weight AS weight, r.temporary AS temporary");
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	ASSERT_TRUE(visible.hasNext());
	visible.next();
	EXPECT_EQ(std::get<int64_t>(visible.get("weight")), 6);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(visible.get("temporary")));
	EXPECT_FALSE(visible.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, CypherSetNullRemovesACreatedEdgePropertyDurably) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	zyx::Result create = database.execute(
			"CREATE (:CreatedEdgeNull)-[r:CREATED_EDGE_NULL {temporary: true}]->(:CreatedEdgeNull) "
			"SET r.temporary = null RETURN r.temporary AS temporary");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	ASSERT_TRUE(create.hasNext());
	create.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(create.get("temporary")));
	EXPECT_FALSE(create.hasNext());
	database.close();

	zyx::Database reopened(data_path_.string());
	reopened.open();
	zyx::Result visible = reopened.execute(
			"MATCH (:CreatedEdgeNull)-[r:CREATED_EDGE_NULL]->(:CreatedEdgeNull) RETURN r.temporary AS temporary");
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	ASSERT_TRUE(visible.hasNext());
	visible.next();
	EXPECT_TRUE(std::holds_alternative<std::monostate>(visible.get("temporary")));
	EXPECT_FALSE(visible.hasNext());
	EXPECT_NO_THROW(reopened.close());
}

TEST_F(RelocatableStorageBackendTest, CypherDeleteUsesTheReadSnapshotFenceAndOneMutationBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode("DeleteSource", {{"name", std::string("Ada")}});
	const int64_t target = database.createNode("DeleteTarget", {{"name", std::string("Bob")}});
	const int64_t edge = database.createEdge(source, target, "DELETE_LINK", {{"weight", int64_t(1)}});
	ASSERT_GT(edge, 0);

	zyx::Result deleteSourceAndEdge = database.execute(
			"MATCH (a:DeleteSource)-[r:DELETE_LINK]->(b) DELETE a, r RETURN a.name AS name, r.weight AS weight");
	ASSERT_TRUE(deleteSourceAndEdge.isSuccess()) << deleteSourceAndEdge.getError();
	ASSERT_TRUE(deleteSourceAndEdge.hasNext());
	deleteSourceAndEdge.next();
	EXPECT_EQ(std::get<std::string>(deleteSourceAndEdge.get("name")), "Ada");
	EXPECT_EQ(std::get<int64_t>(deleteSourceAndEdge.get("weight")), 1);
	zyx::Result sourceCheck = database.execute("MATCH (n:DeleteSource) RETURN n");
	ASSERT_TRUE(sourceCheck.isSuccess()) << sourceCheck.getError();
	EXPECT_FALSE(sourceCheck.hasNext());
	zyx::Result targetCheck = database.execute("MATCH (n:DeleteTarget) RETURN n");
	ASSERT_TRUE(targetCheck.isSuccess()) << targetCheck.getError();
	ASSERT_TRUE(targetCheck.hasNext());
	targetCheck.next();
	EXPECT_EQ(std::get<std::shared_ptr<zyx::Node>>(targetCheck.get("n"))->id, target);

	const zyx::Result deleteTarget = database.execute("MATCH (n:DeleteTarget) DELETE n");
	ASSERT_TRUE(deleteTarget.isSuccess()) << deleteTarget.getError();
	targetCheck = database.execute("MATCH (n:DeleteTarget) RETURN n");
	ASSERT_TRUE(targetCheck.isSuccess()) << targetCheck.getError();
	EXPECT_FALSE(targetCheck.hasNext());

	const int64_t detachSource = database.createNode("DetachSource", {});
	const int64_t detachTarget = database.createNode("DetachTarget", {});
	ASSERT_GT(database.createEdge(detachSource, detachTarget, "DETACH_LINK", {}), 0);
	const zyx::Result detachDelete = database.execute("MATCH (n:DetachSource) DETACH DELETE n");
	ASSERT_TRUE(detachDelete.isSuccess()) << detachDelete.getError();
	zyx::Result detachSourceCheck = database.execute("MATCH (n:DetachSource) RETURN n");
	ASSERT_TRUE(detachSourceCheck.isSuccess()) << detachSourceCheck.getError();
	EXPECT_FALSE(detachSourceCheck.hasNext());
	zyx::Result detachTargetCheck = database.execute("MATCH (n:DetachTarget) RETURN n");
	ASSERT_TRUE(detachTargetCheck.isSuccess()) << detachTargetCheck.getError();
	ASSERT_TRUE(detachTargetCheck.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherSetUsesTheReadSnapshotFenceAndCompletePropertyReplacement) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode("SetSource", {{"score", int64_t(1)}});
	const int64_t target = database.createNode("SetTarget", {});
	ASSERT_GT(database.createEdge(source, target, "SET_LINK", {{"weight", int64_t(1)}}), 0);

	zyx::Result setNode = database.execute("MATCH (n:SetSource) SET n.score = $score RETURN n.score AS score",
										   {{"score", int64_t(7)}});
	ASSERT_TRUE(setNode.isSuccess()) << setNode.getError();
	ASSERT_TRUE(setNode.hasNext());
	setNode.next();
	EXPECT_EQ(std::get<int64_t>(setNode.get("score")), 7);
	const zyx::Result copyNodeProperty = database.execute("MATCH (n:SetSource) SET n.copied = n.score");
	ASSERT_TRUE(copyNodeProperty.isSuccess()) << copyNodeProperty.getError();
	const zyx::Result mergeNodeProperties = database.execute("MATCH (n:SetSource) SET n += {merged: 9, copied: null}");
	ASSERT_TRUE(mergeNodeProperties.isSuccess()) << mergeNodeProperties.getError();
	const zyx::Result setEdge = database.execute("MATCH (a:SetSource)-[r:SET_LINK]->(b) SET r.weight = 2");
	ASSERT_TRUE(setEdge.isSuccess()) << setEdge.getError();
	const zyx::Result setLabel = database.execute("MATCH (n:SetSource) SET n:SetApplied");
	ASSERT_TRUE(setLabel.isSuccess()) << setLabel.getError();

	zyx::Result nodeCheck = database.execute(
			"MATCH (n:SetSource:SetApplied) RETURN n.score AS score, n.copied AS copied, n.merged AS merged");
	ASSERT_TRUE(nodeCheck.isSuccess()) << nodeCheck.getError();
	ASSERT_TRUE(nodeCheck.hasNext());
	nodeCheck.next();
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("score")), 7);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nodeCheck.get("copied")));
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("merged")), 9);
	zyx::Result edgeCheck = database.execute("MATCH (a:SetSource)-[r:SET_LINK]->(b) RETURN r.weight AS weight");
	ASSERT_TRUE(edgeCheck.isSuccess()) << edgeCheck.getError();
	ASSERT_TRUE(edgeCheck.hasNext());
	edgeCheck.next();
	EXPECT_EQ(std::get<int64_t>(edgeCheck.get("weight")), 2);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherMultiRowPropertyWritesCoalesceSharedNativePropertyPages) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("MultiRowProperty", {{"score", int64_t(1)}, {"drop", int64_t(10)}}), 0);
	ASSERT_GT(database.createNode("MultiRowProperty", {{"score", int64_t(2)}, {"drop", int64_t(20)}}), 0);

	zyx::Result set = database.execute("MATCH (n:MultiRowProperty) SET n.score = $score RETURN n.score AS score",
										{{"score", int64_t(9)}});
	ASSERT_TRUE(set.isSuccess()) << set.getError();
	size_t setRows = 0U;
	while (set.hasNext()) {
		set.next();
		EXPECT_EQ(std::get<int64_t>(set.get("score")), 9);
		++setRows;
	}
	EXPECT_EQ(setRows, 2U);

	zyx::Result remove =
			database.execute("MATCH (n:MultiRowProperty) REMOVE n.drop RETURN n.score AS score, n.drop AS drop");
	ASSERT_TRUE(remove.isSuccess()) << remove.getError();
	size_t removeRows = 0U;
	while (remove.hasNext()) {
		remove.next();
		EXPECT_EQ(std::get<int64_t>(remove.get("score")), 9);
		EXPECT_TRUE(std::holds_alternative<std::monostate>(remove.get("drop")));
		++removeRows;
	}
	EXPECT_EQ(removeRows, 2U);
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherRemoveUsesTheReadSnapshotFenceAndCompletePropertyReplacement) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	const int64_t source = database.createNode(std::vector<std::string>{"RemoveSource", "KeepLabel"},
											   {{"keep", int64_t(1)}, {"drop", int64_t(2)}});
	const int64_t target = database.createNode("RemoveTarget", {});
	ASSERT_GT(database.createEdge(source, target, "REMOVE_LINK", {{"keep", int64_t(3)}, {"drop", int64_t(4)}}), 0);

	zyx::Result removeNodeProperty =
			database.execute("MATCH (n:RemoveSource) REMOVE n.drop RETURN n.keep AS keep, n.drop AS drop");
	ASSERT_TRUE(removeNodeProperty.isSuccess()) << removeNodeProperty.getError();
	ASSERT_TRUE(removeNodeProperty.hasNext());
	removeNodeProperty.next();
	EXPECT_EQ(std::get<int64_t>(removeNodeProperty.get("keep")), 1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(removeNodeProperty.get("drop")));
	const zyx::Result removeEdgeProperty =
			database.execute("MATCH (a:RemoveSource)-[r:REMOVE_LINK]->(b) REMOVE r.drop");
	ASSERT_TRUE(removeEdgeProperty.isSuccess()) << removeEdgeProperty.getError();
	const zyx::Result removeNodeLabel = database.execute("MATCH (n:RemoveSource) REMOVE n:RemoveSource");
	ASSERT_TRUE(removeNodeLabel.isSuccess()) << removeNodeLabel.getError();

	zyx::Result nodeCheck = database.execute("MATCH (n:KeepLabel) RETURN n.keep AS keep, n.drop AS drop");
	ASSERT_TRUE(nodeCheck.isSuccess()) << nodeCheck.getError();
	ASSERT_TRUE(nodeCheck.hasNext());
	nodeCheck.next();
	EXPECT_EQ(std::get<int64_t>(nodeCheck.get("keep")), 1);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(nodeCheck.get("drop")));
	zyx::Result edgeCheck =
			database.execute("MATCH (a:KeepLabel)-[r:REMOVE_LINK]->(b) RETURN r.keep AS keep, r.drop AS drop");
	ASSERT_TRUE(edgeCheck.isSuccess()) << edgeCheck.getError();
	ASSERT_TRUE(edgeCheck.hasNext());
	edgeCheck.next();
	EXPECT_EQ(std::get<int64_t>(edgeCheck.get("keep")), 3);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(edgeCheck.get("drop")));
	zyx::Result removedLabelCheck = database.execute("MATCH (n:RemoveSource) RETURN n");
	ASSERT_TRUE(removedLabelCheck.isSuccess()) << removedLabelCheck.getError();
	EXPECT_FALSE(removedLabelCheck.hasNext());
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CypherMatchCreatePublishesAllRowsInOneLogicalBatch) {
	materializeNativeDatabase();

	zyx::Database database(data_path_.string());
	database.open();
	ASSERT_GT(database.createNode("MatchCreateSource", {{"rank", int64_t(3)}}), 0);
	ASSERT_GT(database.createNode("MatchCreateSource", {{"rank", int64_t(5)}}), 0);

	zyx::Result create = database.execute(
			"MATCH (source:MatchCreateSource) "
			"CREATE (source)-[edge:MATCH_CREATED {rank: source.rank}]->(copy:MatchCreateTarget {copied: source.rank}) "
			"RETURN source.rank AS sourceRank, edge.rank AS edgeRank, copy.copied AS copiedRank");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	std::vector<int64_t> returned;
	while (create.hasNext()) {
		create.next();
		const int64_t sourceRank = std::get<int64_t>(create.get("sourceRank"));
		EXPECT_EQ(std::get<int64_t>(create.get("edgeRank")), sourceRank);
		EXPECT_EQ(std::get<int64_t>(create.get("copiedRank")), sourceRank);
		returned.push_back(sourceRank);
	}
	std::ranges::sort(returned);
	EXPECT_EQ(returned, (std::vector<int64_t>{3, 5}));

	zyx::Result check =
			database.execute("MATCH (source:MatchCreateSource)-[edge:MATCH_CREATED]->(copy:MatchCreateTarget) "
							 "RETURN source.rank AS sourceRank, edge.rank AS edgeRank, copy.copied AS copiedRank");
	ASSERT_TRUE(check.isSuccess()) << check.getError();
	std::vector<int64_t> observed;
	while (check.hasNext()) {
		check.next();
		const auto &sourceRankValue = check.get("sourceRank");
		const auto &edgeRankValue = check.get("edgeRank");
		const auto &copiedRankValue = check.get("copiedRank");
		ASSERT_TRUE(std::holds_alternative<int64_t>(sourceRankValue));
		ASSERT_TRUE(std::holds_alternative<int64_t>(edgeRankValue));
		ASSERT_TRUE(std::holds_alternative<int64_t>(copiedRankValue));
		const int64_t sourceRank = std::get<int64_t>(sourceRankValue);
		observed.push_back(sourceRank);
		EXPECT_EQ(std::get<int64_t>(edgeRankValue), sourceRank);
		EXPECT_EQ(std::get<int64_t>(copiedRankValue), sourceRank);
	}
	std::ranges::sort(observed);
	EXPECT_EQ(observed, (std::vector<int64_t>{3, 5}));
	database.close();
}

TEST_F(RelocatableStorageBackendTest, DriverAbiDirectCreatesUseTheLogicalWriteAdapter) {
	materializeNativeDatabase();

	zyx_driver_db_t *database = nullptr;
	zyx_driver_error_t *error = nullptr;
	ASSERT_EQ(zyx_driver_db_open(data_path_.string().c_str(), &database, &error), ZYX_DRIVER_OK);
	ASSERT_NE(database, nullptr);
	ASSERT_EQ(error, nullptr);

	zyx_driver_params_t *properties = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&properties, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_int64(properties, "score", 7, &error), ZYX_DRIVER_OK);
	int64_t source = 0;
	ASSERT_EQ(zyx_driver_db_create_node(database, "DriverOnline", properties, &source, &error), ZYX_DRIVER_OK);
	ASSERT_GT(source, 0);
	zyx_driver_params_free(properties, &error);
	ASSERT_EQ(error, nullptr);
	properties = nullptr;

	int64_t target = 0;
	ASSERT_EQ(zyx_driver_db_create_node(database, "DriverOnline", nullptr, &target, &error), ZYX_DRIVER_OK);
	int64_t edge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(database, source, target, "DRIVER_LINK", nullptr, &edge, &error),
			  ZYX_DRIVER_OK);
	ASSERT_GT(edge, 0);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(database, "MATCH (n:DriverOnline) RETURN n", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	int64_t returned = 0;
	EXPECT_EQ(zyx_driver_result_get_node_id(result, 0U, &returned, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(returned, source);
	zyx_driver_result_free(result);
	zyx_driver_db_close(database, &error);
	EXPECT_EQ(error, nullptr);
}
