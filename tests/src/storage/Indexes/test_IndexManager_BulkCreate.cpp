/**
 * @file test_IndexManager_BulkCreate.cpp
 * @brief Tests coordinated multi-index creation paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

using graph::Edge;
using graph::Node;
using graph::PropertyValue;

class IndexManagerBulkCreateTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_indexManager_bulk_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		graph::debug::PerfTrace::setEnabled(false);
		graph::debug::PerfTrace::reset();
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

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
};

TEST_F(IndexManagerBulkCreateTest, CreateIndexesBuildsScopedNodePropertyIndexesTogether) {
	std::vector<Node> nodes;
	Node user1;
	user1.setLabelId(dataManager->getOrCreateTokenId("User"));
	user1.setProperties({{"id", PropertyValue("user-1")},
						 {"country", PropertyValue("CN")},
						 {"age", PropertyValue(int64_t{31})}});
	nodes.push_back(std::move(user1));

	Node user2;
	user2.setLabelId(dataManager->getOrCreateTokenId("User"));
	user2.setProperties({{"id", PropertyValue("user-2")},
						 {"country", PropertyValue("US")},
						 {"age", PropertyValue(int64_t{42})}});
	nodes.push_back(std::move(user2));

	Node post;
	post.setLabelId(dataManager->getOrCreateTokenId("Post"));
	post.setProperties({{"id", PropertyValue("user-1")}, {"country", PropertyValue("CN")}});
	nodes.push_back(std::move(post));

	dataManager->addNodes(nodes);
	fileStorage->flush();
	dataManager->clearCache();

	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	const auto results = indexManager->createIndexes({
			{"", "node", "User", "id"},
			{"", "node", "User", "country"},
			{"", "node", "User", "age"},
	});
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_EQ(results.size(), 3U);
	EXPECT_TRUE(results[0].success);
	EXPECT_TRUE(results[1].success);
	EXPECT_TRUE(results[2].success);
	EXPECT_TRUE(trace.contains("index_build.node_property.typed_scan"));
	EXPECT_TRUE(trace.contains("index_build.node_property.typed_insert"));

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("User", "id", PropertyValue("user-1")),
			  (std::vector<int64_t>{nodes[0].getId()}));
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("User", "country", PropertyValue("CN")),
			  (std::vector<int64_t>{nodes[0].getId()}));
	EXPECT_EQ(indexManager->countNodeIdsByLabelAndPropertyRange(
			  "User", "age", PropertyValue(int64_t{30}), PropertyValue(int64_t{40})),
			  1U);
	EXPECT_TRUE(indexManager->hasNodePropertyIndexForLabel("User", "id"));
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesRejectsDuplicatePhysicalPropertyInBatch) {
	const auto results = indexManager->createIndexes({
			{"idx_user_id_a", "node", "User", "id"},
			{"idx_user_id_b", "node", "User", "id"},
	});

	ASSERT_EQ(results.size(), 2U);
	EXPECT_TRUE(results[0].success);
	EXPECT_FALSE(results[1].success);
	EXPECT_EQ(results[1].reason, "property index already exists");
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesHandlesEmptyRequestsAndDuplicateBatchNames) {
	EXPECT_TRUE(indexManager->createIndexes({}).empty());

	const auto results = indexManager->createIndexes({
			{"idx_same_name", "node", "User", "id"},
			{"idx_same_name", "edge", "", "weight"},
	});

	ASSERT_EQ(results.size(), 2U);
	EXPECT_TRUE(results[0].success);
	EXPECT_FALSE(results[1].success);
	EXPECT_EQ(results[1].reason, "index name already exists");
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesRejectsDuplicateEdgePhysicalPropertyInBatch) {
	const auto results = indexManager->createIndexes({
			{"idx_edge_weight_a", "edge", "", "weight"},
			{"idx_edge_weight_b", "edge", "", "weight"},
	});

	ASSERT_EQ(results.size(), 2U);
	EXPECT_TRUE(results[0].success);
	EXPECT_FALSE(results[1].success);
	EXPECT_EQ(results[1].reason, "property index already exists");
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesCombinesTypedScanWithBlobFallback) {
	std::vector<Node> nodes;
	Node compactUser;
	compactUser.setLabelId(dataManager->getOrCreateTokenId("User"));
	compactUser.setProperties({{"id", PropertyValue("compact-user")}});
	nodes.push_back(std::move(compactUser));

	Node blobUser;
	blobUser.setLabelId(dataManager->getOrCreateTokenId("User"));
	blobUser.setProperties({{"id", PropertyValue("blob-user")}, {"bio", PropertyValue(std::string(512, 'x'))}});
	nodes.push_back(std::move(blobUser));
	dataManager->addNodes(nodes);
	fileStorage->flush();
	dataManager->clearCache();

	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	const auto results = indexManager->createIndexes({{"idx_user_id_blob_fallback", "node", "User", "id"}});
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_EQ(results.size(), 1U);
	ASSERT_TRUE(results[0].success);
	EXPECT_TRUE(trace.contains("index_build.node_property.typed_scan"));
	EXPECT_TRUE(trace.contains("index_build.node_property.fallback_blob_or_inline"));
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("User", "id", PropertyValue("compact-user")),
			  (std::vector<int64_t>{nodes[0].getId()}));
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("User", "id", PropertyValue("blob-user")),
			  (std::vector<int64_t>{nodes[1].getId()}));
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesBuildsEdgePropertyIndexesTogether) {
	Node a;
	a.setLabelId(dataManager->getOrCreateTokenId("User"));
	Node b;
	b.setLabelId(dataManager->getOrCreateTokenId("User"));
	std::vector<Node> nodes{a, b};
	dataManager->addNodes(nodes);

	std::vector<Edge> edges;
	Edge first(0, nodes[0].getId(), nodes[1].getId(), dataManager->getOrCreateTokenId("FOLLOWS"));
	first.setProperties({{"weight", PropertyValue(int64_t{7})}, {"rank", PropertyValue(int64_t{1})}});
	edges.push_back(std::move(first));
	Edge second(0, nodes[1].getId(), nodes[0].getId(), dataManager->getOrCreateTokenId("FOLLOWS"));
	second.setProperties({{"weight", PropertyValue(int64_t{9})}, {"rank", PropertyValue(int64_t{2})}});
	edges.push_back(std::move(second));
	dataManager->addEdges(edges);
	fileStorage->flush();
	dataManager->clearCache();

	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setEnabled(true);
	const auto results = indexManager->createIndexes({
			{"", "edge", "", "weight"},
			{"", "edge", "", "rank"},
	});
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_EQ(results.size(), 2U);
	EXPECT_TRUE(results[0].success);
	EXPECT_TRUE(results[1].success);
	EXPECT_TRUE(trace.contains("index_build.edge_property.typed_scan"));
	EXPECT_TRUE(trace.contains("index_build.edge_property.typed_insert"));
	EXPECT_EQ(indexManager->findEdgeIdsByProperty("weight", PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{edges[0].getId()}));
	EXPECT_EQ(indexManager->estimateEdgeIdsByProperty("rank", PropertyValue(int64_t{2})), 1U);
}

TEST_F(IndexManagerBulkCreateTest, CreateIndexesSkipsDeletedOwnersInTypedPropertyBuild) {
	std::vector<Node> nodes;
	Node liveNode;
	liveNode.setLabelId(dataManager->getOrCreateTokenId("TypedOwner"));
	liveNode.setProperties({{"name", PropertyValue("live-node")}});
	nodes.push_back(std::move(liveNode));

	Node deletedNode;
	deletedNode.setLabelId(dataManager->getOrCreateTokenId("TypedOwner"));
	deletedNode.setProperties({{"name", PropertyValue("deleted-node")}});
	nodes.push_back(std::move(deletedNode));
	dataManager->addNodes(nodes);
	dataManager->deleteNode(nodes[1]);

	std::vector<Edge> edges;
	Edge liveEdge(0, nodes[0].getId(), nodes[0].getId(), dataManager->getOrCreateTokenId("TYPED_OWNER_EDGE"));
	liveEdge.setProperties({{"kind", PropertyValue("live-edge")}});
	edges.push_back(std::move(liveEdge));

	Edge deletedEdge(0, nodes[0].getId(), nodes[0].getId(), dataManager->getOrCreateTokenId("TYPED_OWNER_EDGE"));
	deletedEdge.setProperties({{"kind", PropertyValue("deleted-edge")}});
	edges.push_back(std::move(deletedEdge));
	dataManager->addEdges(edges);
	dataManager->deleteEdge(edges[1]);

	const auto results = indexManager->createIndexes({
			{"idx_typed_owner_node_name", "node", "", "name"},
			{"idx_typed_owner_edge_kind", "edge", "", "kind"},
	});

	ASSERT_EQ(results.size(), 2U);
	EXPECT_TRUE(results[0].success);
	EXPECT_TRUE(results[1].success);
	EXPECT_EQ(indexManager->findNodeIdsByProperty("name", PropertyValue("live-node")),
			  (std::vector<int64_t>{nodes[0].getId()}));
	EXPECT_TRUE(indexManager->findNodeIdsByProperty("name", PropertyValue("deleted-node")).empty());
	EXPECT_EQ(indexManager->findEdgeIdsByProperty("kind", PropertyValue("live-edge")),
			  (std::vector<int64_t>{edges[0].getId()}));
	EXPECT_TRUE(indexManager->findEdgeIdsByProperty("kind", PropertyValue("deleted-edge")).empty());
}

TEST_F(IndexManagerBulkCreateTest, BatchedNodeObserverMaintainsScopedPropertyIndexes) {
	ASSERT_TRUE(indexManager->createIndex("idx_batch_user_code", "node", "User", "code"));

	const int64_t userLabel = dataManager->getOrCreateTokenId("User");
	const int64_t postLabel = dataManager->getOrCreateTokenId("Post");

	Node matched(100, userLabel);
	matched.setProperties({{"code", PropertyValue(int64_t{7})}});
	Node missingProperty(101, userLabel);
	missingProperty.setProperties({{"other", PropertyValue(int64_t{7})}});
	Node nullProperty(102, userLabel);
	nullProperty.setProperties({{"code", PropertyValue()}});
	Node wrongLabel(103, postLabel);
	wrongLabel.setProperties({{"code", PropertyValue(int64_t{7})}});

	indexManager->onNodesAdded({matched, missingProperty, nullProperty, wrongLabel});

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("User", "code", PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{matched.getId()}));
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty("Post", "code", PropertyValue(int64_t{7})).empty());
}

TEST_F(IndexManagerBulkCreateTest, ColumnarEdgeObserverMaintainsPropertyIndexes) {
	ASSERT_TRUE(indexManager->createIndex("idx_columnar_edge_weight", "edge", "", "weight"));

	const int64_t typeId = dataManager->getOrCreateTokenId("LIKES");
	const std::vector<Edge> edges{
			Edge(200, 1, 2, typeId),
			Edge(201, 2, 1, typeId),
	};
	const std::vector<graph::storage::BulkPropertyColumn> columns{
			{"weight", {PropertyValue(int64_t{3}), PropertyValue(int64_t{5})}},
	};

	indexManager->onEdgesAddedColumnar(edges, columns);

	EXPECT_EQ(indexManager->findEdgeIdsByProperty("weight", PropertyValue(int64_t{3})),
			  (std::vector<int64_t>{edges[0].getId()}));
	EXPECT_EQ(indexManager->estimateEdgeIdsByProperty("weight", PropertyValue(int64_t{5})), 1U);
}

TEST_F(IndexManagerBulkCreateTest, ColumnarNodeObserverMaintainsCompositeIndex) {
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_columnar_comp", "node", "User", {"name", "age"}));

	const int64_t userLabel = dataManager->getOrCreateTokenId("User");
	const std::vector<Node> nodes{
			Node(300, userLabel),
			Node(301, userLabel),
	};
	const std::vector<graph::storage::BulkPropertyColumn> columns{
			{"name", {PropertyValue("Alice"), PropertyValue("Bob")}},
			{"age", {PropertyValue(int64_t{30}), PropertyValue()}},
	};

	indexManager->onNodesAddedColumnar(nodes, columns);

	EXPECT_EQ(indexManager->findNodeIdsByCompositeIndex(
					  {"name", "age"},
					  {PropertyValue("Alice"), PropertyValue(int64_t{30})}),
			  (std::vector<int64_t>{nodes[0].getId()}));
	EXPECT_TRUE(indexManager->findNodeIdsByCompositeIndex(
					 {"name", "age"},
					 {PropertyValue("Bob"), PropertyValue()}).empty());
}
