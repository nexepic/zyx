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

TEST_F(NodeCandidateSourceTest, LabelScanMetadataMarksActiveAndLabelSatisfied) {
	EXPECT_TRUE(indexManager->createIndex("idx_node_labels_meta", "node", "", ""));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	Node node(1, personLabel);
	indexManager->onNodeAdded(node);

	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.labels = {"Person"};

	auto candidates = makeSource().collectWithMetadata(config);

	EXPECT_EQ(candidates.ids, (std::vector<int64_t>{1}));
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.labelsSatisfied);
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

TEST_F(NodeCandidateSourceTest, PropertyScanUsesScopedLabelIndexAndMarksLabelsSatisfied) {
	EXPECT_TRUE(indexManager->createIndex("idx_person_name_meta", "node", "Person", "name"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	const int64_t animalLabel = dataManager->getOrCreateTokenId("Animal");
	Node alice(1, personLabel);
	Node animal(2, animalLabel);
	dataManager->addNode(alice);
	dataManager->addNode(animal);
	dataManager->addNodeProperties(1, {{"name", std::string("Alice")}});
	dataManager->addNodeProperties(2, {{"name", std::string("Alice")}});

	NodeScanConfig config;
	config.type = ScanType::PROPERTY_SCAN;
	config.labels = {"Person"};
	config.indexKey = "name";
	config.indexValue = PropertyValue(std::string("Alice"));

	auto candidates = makeSource().collectWithMetadata(config);

	EXPECT_EQ(candidates.ids, (std::vector<int64_t>{1}));
	EXPECT_TRUE(candidates.activeOnly);
	EXPECT_TRUE(candidates.labelsSatisfied);
}

TEST_F(NodeCandidateSourceTest, CountUsesScopedLabelIndexWithoutMaterializingIds) {
	EXPECT_TRUE(indexManager->createIndex("idx_person_score_count", "node", "Person", "score"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	const int64_t animalLabel = dataManager->getOrCreateTokenId("Animal");
	for (int64_t id = 1; id <= 6; ++id) {
		Node node(id, id <= 4 ? personLabel : animalLabel);
		dataManager->addNode(node);
		dataManager->addNodeProperties(id, {{"score", PropertyValue(id)}});
	}

	NodeScanConfig config;
	config.type = ScanType::RANGE_SCAN;
	config.labels = {"Person"};
	config.indexKey = "score";
	config.rangeMin = PropertyValue(int64_t{2});
	config.rangeMax = PropertyValue(int64_t{5});

	auto count = makeSource().countWithMetadata(config);

	EXPECT_TRUE(count.available);
	EXPECT_TRUE(count.activeOnly);
	EXPECT_TRUE(count.labelsSatisfied);
	EXPECT_EQ(count.count, 3);
}

TEST_F(NodeCandidateSourceTest, RangeScanHonorsExclusiveBounds) {
	EXPECT_TRUE(indexManager->createIndex("idx_person_age", "node", "Person", "age"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	for (int64_t id = 1; id <= 4; ++id) {
		Node node(id, personLabel);
		dataManager->addNode(node);
		dataManager->addNodeProperties(id, {{"age", id * 10}});
	}

	NodeScanConfig config;
	config.type = ScanType::RANGE_SCAN;
	config.indexKey = "age";
	config.rangeMin = PropertyValue(int64_t{10});
	config.rangeMax = PropertyValue(int64_t{40});
	config.minInclusive = false;
	config.maxInclusive = false;

	auto candidates = makeSource().collect(config);

	EXPECT_EQ(candidates, (std::vector<int64_t>{2, 3}));
}
