/**
 * @file test_IndexBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Database.hpp"
#define private public
#include "graph/query/indexes/IndexBuilder.hpp"
#undef private
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/storage/FileStorage.hpp"

class IndexBuilderTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_indexBuilder_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		indexManager = database->getQueryEngine()->getIndexManager();
		indexBuilder = indexManager->getIndexBuilder();
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	graph::query::indexes::IndexBuilder *indexBuilder = nullptr;
};

TEST_F(IndexBuilderTest, BuildAllIndexes) { EXPECT_TRUE(indexBuilder->buildAllIndexes()); }

TEST_F(IndexBuilderTest, BuildLabelIndex) { EXPECT_TRUE(indexBuilder->buildLabelIndex()); }

TEST_F(IndexBuilderTest, BuildPropertyIndex) { EXPECT_TRUE(indexBuilder->buildPropertyIndex("name")); }

TEST_F(IndexBuilderTest, GetNodeAndEdgeIdRangesEmpty) {
	// Should return empty ranges if no segments
	auto nodeRanges = indexBuilder->getNodeIdRanges();
	auto edgeRanges = indexBuilder->getEdgeIdRanges();
	EXPECT_TRUE(nodeRanges.empty());
	EXPECT_TRUE(edgeRanges.empty());
}

TEST_F(IndexBuilderTest, GetNodeAndEdgeIdRangesNonEmpty) {
	// Add a node and edge, then check ranges
	graph::Node node1(1, "Node1");
	graph::Node node2(2, "Node2");
	fileStorage->getDataManager()->addNode(node1);
	fileStorage->getDataManager()->addNode(node2);

	// Add an edge between the two nodes
	graph::Edge edge(1, node1.getId(), node2.getId(), "KNOWS");
	fileStorage->getDataManager()->addEdge(edge);
	fileStorage->save();
	auto nodeRanges = indexBuilder->getNodeIdRanges();
	auto edgeRanges = indexBuilder->getEdgeIdRanges();
	EXPECT_FALSE(nodeRanges.empty());
	EXPECT_FALSE(edgeRanges.empty());
}
