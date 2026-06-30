/**
 * @file test_NativeBulkLoader.cpp
 * @brief Tests storage-native bulk ingest and deferred index construction.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/data/NativeBulkLoader.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace {

	class NativeBulkLoaderTest : public ::testing::Test {
	protected:
		void SetUp() override {
			dbPath = std::filesystem::temp_directory_path() /
					 ("zyx_native_bulk_loader_" + std::to_string(++counter) + ".db");
			removeArtifacts();
		}

		void TearDown() override { removeArtifacts(); }

		void removeArtifacts() {
			std::error_code ignored;
			std::filesystem::remove_all(dbPath, ignored);
			std::filesystem::remove_all(std::filesystem::path(dbPath.string() + "-wal"), ignored);
		}

		std::filesystem::path dbPath;
		static inline int counter = 0;
	};

} // namespace

TEST_F(NativeBulkLoaderTest, LoadsColumnarNodesEdgesAndBuildsDeferredIndexes) {
	graph::Database db(dbPath.string());
	db.open();

	auto loadTxn = db.beginBulkTransaction();
	auto dm = db.getStorage()->getDataManager();
	graph::storage::NativeBulkLoader loader(dm);

	std::vector<graph::storage::BulkPropertyColumn> nodeColumns{
			{"id", {graph::PropertyValue("u1"), graph::PropertyValue("u2"), graph::PropertyValue("u3")}},
			{"score", {graph::PropertyValue(int64_t{10}), graph::PropertyValue(int64_t{20}),
						   graph::PropertyValue(int64_t{30})}}};
	const auto nodeIds = loader.addNodes("NativeBulkUser", nodeColumns.front().values.size(), nodeColumns);
	ASSERT_EQ(nodeIds.size(), 3U);
	const auto cachedTokenNodeIds = loader.addNodes("NativeBulkUser", 1, {{"id", {graph::PropertyValue("u4")}}});
	ASSERT_EQ(cachedTokenNodeIds.size(), 1U);

	std::vector<graph::storage::BulkPropertyColumn> edgeColumns{
			{"weight", {graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{11})}}};
	const auto edgeIds = loader.addEdges(
			"NATIVE_FOLLOWS",
			std::vector<int64_t>{nodeIds[0], nodeIds[1]},
			std::vector<int64_t>{nodeIds[1], nodeIds[2]},
			edgeColumns);
	ASSERT_EQ(edgeIds.size(), 2U);
	EXPECT_EQ(loader.stats().nodeRows, 4U);
	EXPECT_EQ(loader.stats().edgeRows, 2U);
	loadTxn.commit();

	db.flush();
	auto queryEngine = db.getQueryEngine();
	ASSERT_NE(queryEngine, nullptr);

	loader.deferNodePropertyIndexes("NativeBulkUser", {"id", "score", "id"});
	loader.deferEdgePropertyIndexes({"weight", "weight"});
	EXPECT_TRUE(loader.hasDeferredIndexes());
	EXPECT_EQ(loader.stats().deferredIndexRequests, 3U);

	auto schemaTxn = db.beginBulkTransaction();
	const auto results = loader.buildDeferredIndexes(*queryEngine->getIndexManager());
	ASSERT_EQ(results.size(), 3U);
	EXPECT_TRUE(results[0].success);
	EXPECT_TRUE(results[1].success);
	EXPECT_TRUE(results[2].success);
	schemaTxn.commit();
	EXPECT_FALSE(loader.hasDeferredIndexes());
	EXPECT_EQ(loader.stats().deferredIndexRequests, 0U);

	const auto idMatches = queryEngine->getIndexManager()->findNodeIdsByLabelAndProperty(
			"NativeBulkUser", "id", graph::PropertyValue("u2"));
	ASSERT_EQ(idMatches.size(), 1U);
	EXPECT_EQ(idMatches.front(), nodeIds[1]);

	const auto scoreMatches = queryEngine->getIndexManager()->findNodeIdsByLabelAndProperty(
			"NativeBulkUser", "score", graph::PropertyValue(int64_t{30}));
	ASSERT_EQ(scoreMatches.size(), 1U);
	EXPECT_EQ(scoreMatches.front(), nodeIds[2]);

	const auto edgeMatches = queryEngine->getIndexManager()->findEdgeIdsByProperty(
			"weight", graph::PropertyValue(int64_t{11}));
	ASSERT_EQ(edgeMatches.size(), 1U);
	EXPECT_EQ(edgeMatches.front(), edgeIds[1]);

	const auto storedEdge = dm->getEdge(edgeIds[0]);
	EXPECT_TRUE(storedEdge.isActive());
	EXPECT_EQ(storedEdge.getSourceNodeId(), nodeIds[0]);
	EXPECT_EQ(storedEdge.getTargetNodeId(), nodeIds[1]);

	db.close();
}

TEST_F(NativeBulkLoaderTest, RejectsInvalidConstructionAndDeferredIndexRequests) {
	EXPECT_THROW((void) graph::storage::NativeBulkLoader(nullptr), std::invalid_argument);

	graph::Database db(dbPath.string());
	db.open();
	graph::storage::NativeBulkLoader loader(db.getStorage()->getDataManager());

	EXPECT_THROW((void) loader.addNodes("", 0, {}), std::invalid_argument);
	EXPECT_THROW(loader.deferNodePropertyIndexes("", {"id"}), std::invalid_argument);
	EXPECT_THROW(loader.deferNodePropertyIndexes("User", {""}), std::invalid_argument);
	EXPECT_THROW(loader.deferEdgePropertyIndexes({""}), std::invalid_argument);
	EXPECT_TRUE(loader.buildDeferredIndexes(*db.getQueryEngine()->getIndexManager()).empty());

	db.close();
}

TEST_F(NativeBulkLoaderTest, KeepsDeferredIndexesWhenBuildReportsFailures) {
	graph::Database db(dbPath.string());
	db.open();
	auto queryEngine = db.getQueryEngine();
	ASSERT_NE(queryEngine, nullptr);
	ASSERT_TRUE(queryEngine->getIndexManager()->createIndex("existing_user_id", "node", "User", "id"));

	graph::storage::NativeBulkLoader loader(db.getStorage()->getDataManager());
	loader.deferNodePropertyIndexes("User", {"id"});
	const auto results = loader.buildDeferredIndexes(*queryEngine->getIndexManager());

	ASSERT_EQ(results.size(), 1U);
	EXPECT_FALSE(results.front().success);
	EXPECT_EQ(results.front().reason, "property index already exists");
	EXPECT_TRUE(loader.hasDeferredIndexes());
	EXPECT_EQ(loader.stats().deferredIndexRequests, 1U);

	db.close();
}
