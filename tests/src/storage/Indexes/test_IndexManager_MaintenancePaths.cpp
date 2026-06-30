/**
 * @file test_IndexManager_MaintenancePaths.cpp
 * @brief IndexManager schema validation and incremental maintenance behavior.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/ColumnarBulkInput.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

namespace {

using graph::Edge;
using graph::Node;
using graph::PropertyValue;
using graph::query::indexes::IndexManager;
using graph::storage::BulkPropertyColumn;

class IndexManagerMaintenanceTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_index_manager_maintenance_" + boost::uuids::to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		indexManager.reset();
		dataManager.reset();
		fileStorage.reset();
		if (database) {
			database->close();
		}
		database.reset();
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	Node makeNode(int64_t id, const std::string &label) const {
		Node node(id, dataManager->getOrCreateTokenId(label));
		return node;
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<IndexManager> indexManager;
};

} // namespace

TEST_F(IndexManagerMaintenanceTest, CreateIndexRejectsUnsupportedTypesAndExistingPhysicalKeys) {
	EXPECT_FALSE(indexManager->createIndex("bad_label", "relationship", "", ""));
	EXPECT_FALSE(indexManager->createIndex("bad_property", "relationship", "", "weight"));

	ASSERT_TRUE(indexManager->createIndex("user_id", "node", "User", "id"));
	EXPECT_FALSE(indexManager->createIndex("user_id_again", "node", "User", "id"));

	ASSERT_TRUE(indexManager->createIndex("edge_weight", "edge", "", "weight"));
	EXPECT_FALSE(indexManager->createIndex("edge_weight_again", "edge", "", "weight"));
}

TEST_F(IndexManagerMaintenanceTest, SingleNodeMaintenanceUpdatesScopedCompositeAndVectorIndexes) {
	ASSERT_TRUE(indexManager->createCompositeIndex("compound_name_age", "node", "", {"name", "age"}));
	ASSERT_TRUE(indexManager->createIndex("maint_user_code", "node", "MaintUser", "code"));
	ASSERT_TRUE(indexManager->createVectorIndex("maint_embedding", "MaintUser", "embedding", 2, "cosine"));

	Node node = makeNode(1, "MaintUser");
	node.setProperties({{"name", PropertyValue("alice")},
						{"age", PropertyValue(int64_t{30})},
						{"code", PropertyValue("u1")},
						{"embedding", PropertyValue(std::vector<PropertyValue>{
											  PropertyValue(0.1), PropertyValue(0.2)})}});
	indexManager->onNodeAdded(node);

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("MaintUser", "code", PropertyValue("u1")),
			  (std::vector<int64_t>{1}));
	EXPECT_EQ(indexManager->findNodeIdsByCompositeIndex(
					  {"name", "age"}, {PropertyValue("alice"), PropertyValue(int64_t{30})}),
			  (std::vector<int64_t>{1}));
	EXPECT_EQ(indexManager->getVectorIndexName("MaintUser", "embedding"), "maint_embedding");

	Node updated = node;
	updated.setProperties({{"name", PropertyValue("alice")},
						   {"age", PropertyValue(int64_t{31})},
						   {"code", PropertyValue("u2")}});
	indexManager->onNodeUpdated(node, updated);
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty("MaintUser", "code", PropertyValue("u1")).empty());
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("MaintUser", "code", PropertyValue("u2")),
			  (std::vector<int64_t>{1}));

	// Same entity state is intentionally ignored by incremental maintenance.
	indexManager->onNodeUpdated(updated, updated);
	Edge edge(7, 1, 1, dataManager->getOrCreateTokenId("SAME_EDGE"));
	indexManager->onEdgeUpdated(edge, edge);

	indexManager->onNodeDeleted(updated);
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty("MaintUser", "code", PropertyValue("u2")).empty());
	EXPECT_TRUE(indexManager->findNodeIdsByCompositeIndex(
			{"name", "age"}, {PropertyValue("alice"), PropertyValue(int64_t{31})}).empty());

	Node differentId = updated;
	differentId.setId(updated.getId() + 100);
	indexManager->onNodeUpdated(updated, differentId);
	Node inactive = updated;
	inactive.markInactive();
	indexManager->onNodeUpdated(updated, inactive);

	Edge differentEdgeId = edge;
	differentEdgeId.setId(edge.getId() + 100);
	indexManager->onEdgeUpdated(edge, differentEdgeId);
	Edge inactiveEdge = edge;
	inactiveEdge.markInactive();
	indexManager->onEdgeUpdated(edge, inactiveEdge);
}

TEST_F(IndexManagerMaintenanceTest, BatchNodeMaintenanceSkipsRowsThatCannotProduceIndexEntries) {
	ASSERT_TRUE(indexManager->createCompositeIndex("batch_ab", "node", "", {"a", "b"}));
	ASSERT_TRUE(indexManager->createIndex("batch_scoped_a", "node", "BatchScoped", "a"));

	Node zero = makeNode(0, "BatchScoped");
	zero.setProperties({{"a", PropertyValue(int64_t{1})}, {"b", PropertyValue(int64_t{2})}});

	Node inactive = makeNode(2, "BatchScoped");
	inactive.setProperties({{"a", PropertyValue(int64_t{3})}, {"b", PropertyValue(int64_t{4})}});
	inactive.markInactive();

	Node missingProperty = makeNode(3, "BatchScoped");
	missingProperty.setProperties({{"a", PropertyValue(int64_t{5})}});

	Node nullProperty = makeNode(4, "BatchScoped");
	nullProperty.setProperties({{"a", PropertyValue(int64_t{6})}, {"b", PropertyValue()}});

	Node valid = makeNode(5, "BatchScoped");
	valid.setProperties({{"a", PropertyValue(int64_t{7})}, {"b", PropertyValue(int64_t{8})}});

	indexManager->onNodesAdded({zero, inactive, missingProperty, nullProperty, valid});

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("BatchScoped", "a", PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{5}));
	EXPECT_EQ(indexManager->findNodeIdsByCompositeIndex(
					  {"a", "b"}, {PropertyValue(int64_t{7}), PropertyValue(int64_t{8})}),
			  (std::vector<int64_t>{5}));
	EXPECT_TRUE(indexManager->findNodeIdsByCompositeIndex(
			{"a", "b"}, {PropertyValue(int64_t{6}), PropertyValue()}).empty());
}

TEST_F(IndexManagerMaintenanceTest, ColumnarNodeMaintenanceHandlesSparseColumnsAndInactiveRows) {
	ASSERT_TRUE(indexManager->createCompositeIndex("columnar_ab", "node", "", {"a", "b"}));
	ASSERT_TRUE(indexManager->createIndex("columnar_scoped_a", "node", "ColumnarScoped", "a"));

	const int64_t labelId = dataManager->getOrCreateTokenId("ColumnarScoped");
	std::vector<Node> nodes;
	nodes.emplace_back(10, labelId);
	nodes.emplace_back(11, labelId);
	nodes.emplace_back(12, labelId);
	nodes.back().markInactive();
	nodes.emplace_back(13, 0);
	nodes.emplace_back(14, labelId);

	const std::vector<BulkPropertyColumn> sparseColumns = {
			{"a", {PropertyValue(int64_t{1}), PropertyValue(), PropertyValue(int64_t{3}), PropertyValue(int64_t{9})}},
			{"b", {PropertyValue(int64_t{2})}},
	};
	indexManager->onNodesAddedColumnar(nodes, sparseColumns);

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("ColumnarScoped", "a", PropertyValue(int64_t{1})),
			  (std::vector<int64_t>{10}));
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty("ColumnarScoped", "a", PropertyValue(int64_t{3})).empty());
	EXPECT_EQ(indexManager->findNodeIdsByCompositeIndex(
					  {"a", "b"}, {PropertyValue(int64_t{1}), PropertyValue(int64_t{2})}),
			  (std::vector<int64_t>{10}));

	EXPECT_NO_THROW(indexManager->onNodesAddedColumnar({}, sparseColumns));
	EXPECT_NO_THROW(indexManager->onNodesAddedColumnar(nodes, {}));
}
