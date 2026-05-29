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

TEST_F(NodeCandidateSourceTest, CountFallsBackToUnscopedPropertyAndRangeIndexes) {
	EXPECT_TRUE(indexManager->createIndex("idx_node_age_unscoped_count", "node", "", "age"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	for (int64_t id = 1; id <= 3; ++id) {
		Node node(id, personLabel);
		dataManager->addNode(node);
		dataManager->addNodeProperties(id, {{"age", PropertyValue(id * 10)}});
	}

	NodeScanConfig propertyConfig;
	propertyConfig.type = ScanType::PROPERTY_SCAN;
	propertyConfig.indexKey = "age";
	propertyConfig.indexValue = PropertyValue(int64_t{20});
	auto propertyCount = makeSource().countWithMetadata(propertyConfig);
	EXPECT_TRUE(propertyCount.available);
	EXPECT_TRUE(propertyCount.activeOnly);
	EXPECT_TRUE(propertyCount.labelsSatisfied);
	EXPECT_EQ(propertyCount.count, 1);

	NodeScanConfig rangeConfig;
	rangeConfig.type = ScanType::RANGE_SCAN;
	rangeConfig.indexKey = "age";
	rangeConfig.rangeMin = PropertyValue(int64_t{10});
	rangeConfig.rangeMax = PropertyValue(int64_t{30});
	auto rangeCount = makeSource().countWithMetadata(rangeConfig);
	EXPECT_TRUE(rangeCount.available);
	EXPECT_TRUE(rangeCount.activeOnly);
	EXPECT_TRUE(rangeCount.labelsSatisfied);
	EXPECT_EQ(rangeCount.count, 3);
}

TEST_F(NodeCandidateSourceTest, MultiLabelScanIntersectsAndStopsOnEmptyCandidates) {
	EXPECT_TRUE(indexManager->createIndex("idx_node_labels_multi_intersection", "node", "", ""));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	const int64_t engineerLabel = dataManager->getOrCreateTokenId("Engineer");
	const int64_t animalLabel = dataManager->getOrCreateTokenId("Animal");

	Node person(1, personLabel);
	Node engineer(1, engineerLabel);
	Node animal(2, animalLabel);
	indexManager->onNodeAdded(person);
	indexManager->onNodeAdded(engineer);
	indexManager->onNodeAdded(animal);

	NodeScanConfig matching;
	matching.type = ScanType::LABEL_SCAN;
	matching.labels = {"Person", "Engineer"};
	auto matched = makeSource().collectWithMetadata(matching);
	EXPECT_EQ(matched.ids, (std::vector<int64_t>{1}));
	EXPECT_TRUE(matched.labelsSatisfied);

	NodeScanConfig disjoint;
	disjoint.type = ScanType::LABEL_SCAN;
	disjoint.labels = {"Person", "Animal", "Engineer"};
	auto empty = makeSource().collectWithMetadata(disjoint);
	EXPECT_TRUE(empty.ids.empty());
	EXPECT_TRUE(empty.labelsSatisfied);
}

TEST_F(NodeCandidateSourceTest, CompositeAndMultiLabelCountsExposeAvailability) {
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_node_city_age_count", "node", "Person", {"city", "age"}));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");
	Node alice(1, personLabel);
	Node bob(2, personLabel);
	dataManager->addNode(alice);
	dataManager->addNode(bob);
	dataManager->addNodeProperties(1, {{"city", PropertyValue(std::string("NYC"))}, {"age", PropertyValue(int64_t{30})}});
	dataManager->addNodeProperties(2, {{"city", PropertyValue(std::string("SFO"))}, {"age", PropertyValue(int64_t{40})}});

	NodeScanConfig composite;
	composite.type = ScanType::COMPOSITE_SCAN;
	composite.compositeKeys = {"city", "age"};
	composite.compositeValues = {PropertyValue(std::string("NYC")), PropertyValue(int64_t{30})};
	auto compositeCount = makeSource().countWithMetadata(composite);
	EXPECT_TRUE(compositeCount.available);
	EXPECT_TRUE(compositeCount.activeOnly);
	EXPECT_TRUE(compositeCount.labelsSatisfied);
	EXPECT_EQ(compositeCount.count, 1);

	NodeScanConfig multiLabelCount;
	multiLabelCount.type = ScanType::LABEL_SCAN;
	multiLabelCount.labels = {"Person", "Engineer"};
	auto unavailable = makeSource().countWithMetadata(multiLabelCount);
	EXPECT_FALSE(unavailable.available);
	EXPECT_EQ(unavailable.count, 0);
}

TEST_F(NodeCandidateSourceTest, CountsHandleCompositeDuplicatesLabeledFallbackAndFullScan) {
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_node_role_team_count", "node", "Person", {"role", "team"}));
	ASSERT_TRUE(indexManager->createIndex("idx_node_score_unscoped_for_label_fallback", "node", "", "score"));
	const int64_t personLabel = dataManager->getOrCreateTokenId("Person");

	for (int64_t id = 1; id <= 2; ++id) {
		Node node(id, personLabel);
		dataManager->addNode(node);
		dataManager->addNodeProperties(id, {
			{"role", PropertyValue(std::string("engineer"))},
			{"team", PropertyValue(std::string("storage"))},
			{"score", PropertyValue(id * 10)}
		});
	}

	NodeScanConfig composite;
	composite.type = ScanType::COMPOSITE_SCAN;
	composite.compositeKeys = {"role", "team"};
	composite.compositeValues = {PropertyValue(std::string("engineer")), PropertyValue(std::string("storage"))};
	auto compositeCount = makeSource().countWithMetadata(composite);
	EXPECT_TRUE(compositeCount.available);
	EXPECT_EQ(compositeCount.count, 2);

	NodeScanConfig labeledRange;
	labeledRange.type = ScanType::RANGE_SCAN;
	labeledRange.labels = {"Person"};
	labeledRange.indexKey = "score";
	labeledRange.rangeMin = PropertyValue(int64_t{10});
	labeledRange.rangeMax = PropertyValue(int64_t{20});
	auto fallbackCount = makeSource().countWithMetadata(labeledRange);
	EXPECT_TRUE(fallbackCount.available);
	EXPECT_FALSE(fallbackCount.labelsSatisfied);
	EXPECT_EQ(fallbackCount.count, 2);

	NodeScanConfig fullScan;
	fullScan.type = ScanType::FULL_SCAN;
	auto unavailable = makeSource().countWithMetadata(fullScan);
	EXPECT_FALSE(unavailable.available);
	EXPECT_EQ(unavailable.count, 0);

	NodeScanConfig invalidType;
	invalidType.type = static_cast<ScanType>(999);
	auto invalidUnavailable = makeSource().countWithMetadata(invalidType);
	EXPECT_FALSE(invalidUnavailable.available);
	EXPECT_EQ(invalidUnavailable.count, 0);
}

TEST_F(NodeCandidateSourceTest, MultiLabelScanWithoutLabelIndexLeavesLabelsUnsatisfied) {
	NodeScanConfig config;
	config.type = ScanType::LABEL_SCAN;
	config.labels = {"Person", "Engineer"};

	auto candidates = makeSource().collectWithMetadata(config);

	EXPECT_TRUE(candidates.ids.empty());
	EXPECT_FALSE(candidates.labelsSatisfied);
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
