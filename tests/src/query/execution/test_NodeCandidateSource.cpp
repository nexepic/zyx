/**
 * @file test_NodeCandidateSource.cpp
 * @date 2026/05/26
 *
 * Licensed under the Apache License, Version 2.0.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/ScanConfigs.hpp"

namespace fs = std::filesystem;

using namespace graph;
using namespace graph::query::execution;

class NodeCandidateSourceTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_node_candidate_source_" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		indexManager.reset();
		dataManager.reset();
		if (database) {
			database->close();
		}
		database.reset();

		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	[[nodiscard]] NodeCandidateSource makeSource() const {
		return NodeCandidateSource(dataManager, indexManager);
	}

	fs::path testFilePath;
	std::unique_ptr<Database> database;
	std::shared_ptr<storage::DataManager> dataManager;
	std::shared_ptr<query::indexes::IndexManager> indexManager;
};

TEST_F(NodeCandidateSourceTest, FullScanReturnsAllocatedIdsInOrder) {
	const int64_t labelId = dataManager->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	Node n3(0, labelId);
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;

	auto candidates = makeSource().collect(config);

	EXPECT_EQ(candidates, (std::vector<int64_t>{1, 2, 3}));
}

TEST_F(NodeCandidateSourceTest, LabelScanReturnsSortedUniqueIds) {
	EXPECT_TRUE(indexManager->createIndex("idx_node_labels", "node", "", ""));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	Node n3(3, personLabel);
	Node n1(1, personLabel);
	Node n2(2, personLabel);
	indexManager->onNodeAdded(n3);
	indexManager->onNodeAdded(n1);
	indexManager->onNodeAdded(n2);
	indexManager->onNodeAdded(n1);

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.labels = {"Person"};

	auto candidates = makeSource().collect(config);

	EXPECT_EQ(candidates, (std::vector<int64_t>{1, 2, 3}));
}

TEST_F(NodeCandidateSourceTest, MissingLabelScanReturnsEmpty) {
	EXPECT_TRUE(indexManager->createIndex("idx_node_labels", "node", "", ""));

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.labels = {"Missing"};

	auto candidates = makeSource().collect(config);

	EXPECT_TRUE(candidates.empty());
}

TEST_F(NodeCandidateSourceTest, PropertyScanReturnsMatchingIds) {
	EXPECT_TRUE(indexManager->createIndex("idx_person_name", "node", "Person", "name"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	Node alice(1, personLabel);
	Node bob(2, personLabel);
	dataManager->addNode(alice);
	dataManager->addNode(bob);
	dataManager->addNodeProperties(1, {{"name", std::string("Alice")}});
	dataManager->addNodeProperties(2, {{"name", std::string("Bob")}});

	NodeScanConfig config;
	config.type = ScanType::PROPERTY_SCAN;
	config.indexKey = "name";
	config.indexValue = PropertyValue(std::string("Alice"));

	auto candidates = makeSource().collect(config);

	EXPECT_EQ(candidates, (std::vector<int64_t>{1}));
}
