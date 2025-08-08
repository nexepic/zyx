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
#include "graph/query/indexes/IndexBuilder.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/storage/FileStorage.hpp"

constexpr size_t BATCH_SIZE = 1000;

class IndexBuilderTest : public ::testing::Test {
protected:
	// Use SetUpTestSuite and TearDownTestSuite for one-time path generation and cleanup.
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_indexBuilder_" + to_string(uuid) + ".dat");
	}

	static void TearDownTestSuite() {
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// SetUp is called before each test.
	void SetUp() override {
		// Ensure a clean state by deleting the file before each test.
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
		// Open a fresh database instance for the test.
		open_database();
	}

	// TearDown is called after each test.
	void TearDown() override {
		// Ensure the database is closed and resources are freed.
		close_database();
	}

	// --- Helper functions for managing database lifecycle ---

	void open_database() {
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
		indexBuilder = indexManager->getIndexBuilder();
	}

	void close_database() {
		if (database) {
			database->close(); // This is the crucial step that closes file handles.
			database.reset();
		}
		// Nullify pointers to prevent accidental use after close.
		fileStorage.reset();
		dataManager.reset();
		indexManager.reset();
		indexBuilder = nullptr;
	}

	// Helper function to populate the database with comprehensive test data
	void populateDatabaseForTests() const {
		// Add active node 1 ("Alice")
		graph::Node node1(1, "Person");
		dataManager->addNode(node1);
		dataManager->addNodeProperties(1, {{"name", std::string("Alice")}, {"age", static_cast<int64_t>(30)}});

		// Add active node 2 ("Bob")
		graph::Node node2(2, "Person");
		dataManager->addNode(node2);
		dataManager->addNodeProperties(2, {{"name", std::string("Bob")}, {"skill", std::string("coding")}});

		// Add active node 3 ("Graph Inc.")
		graph::Node node3(3, "Company");
		dataManager->addNode(node3);
		dataManager->addNodeProperties(3, {{"name", std::string("Graph Inc.")}});

		// Add an inactive node, which should not be indexed
		graph::Node inactiveNode(4, "Person");
		dataManager->addNode(inactiveNode);
		dataManager->addNodeProperties(4, {{"name", std::string("InactiveUser")}});
		fileStorage->flush();
		dataManager->deleteNode(inactiveNode); // Pass the object, not ID

		// Add active edge 1
		graph::Edge edge1(1, 1, 2, "KNOWS");
		dataManager->addEdge(edge1);
		dataManager->addEdgeProperties(1, {{"since", static_cast<int64_t>(2020)}, {"active", true}});

		// Add active edge 2
		graph::Edge edge2(2, 1, 3, "WORKS_AT");
		dataManager->addEdge(edge2);

		// Add an inactive edge, which should not be indexed
		graph::Edge inactiveEdge(3, 2, 3, "KNOWS");
		dataManager->addEdge(inactiveEdge);
		fileStorage->flush();
		dataManager->deleteEdge(inactiveEdge); // Pass the object, not ID
		fileStorage->flush();
	}

	// Use a static path to be shared across SetUp and TearDown.
	static inline std::filesystem::path testFilePath;

	// Member variables for a single test run.
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	graph::query::indexes::IndexBuilder *indexBuilder = nullptr;
};

// Test 1: Empty database scenario
TEST_F(IndexBuilderTest, BuildAllIndexes_OnEmptyDatabase) {
	// Act: Call entity-specific build methods.
	EXPECT_TRUE(indexBuilder->buildAllNodeIndexes());
	EXPECT_TRUE(indexBuilder->buildAllEdgeIndexes());

	// Assert: Verify through the specific entity index managers.
	auto nodeLabelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	auto edgeLabelIndex = indexManager->getEdgeIndexManager()->getLabelIndex();
	auto edgePropertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();

	EXPECT_TRUE(nodeLabelIndex->isEmpty());
	EXPECT_TRUE(nodePropertyIndex->isEmpty());
	EXPECT_TRUE(edgeLabelIndex->isEmpty());
	EXPECT_TRUE(edgePropertyIndex->isEmpty());
}

// Test 2: Verify buildAllNodeIndexes and buildAllEdgeIndexes functionality and correctness
TEST_F(IndexBuilderTest, BuildAllIndexes_WithData_Verification) {
	// Arrange
	populateDatabaseForTests();

	// Act
	EXPECT_TRUE(indexBuilder->buildAllNodeIndexes());
	EXPECT_TRUE(indexBuilder->buildAllEdgeIndexes());

	// Assert: --- Verify Node Indexes ---
	auto nodeLabelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();

	auto personNodes = nodeLabelIndex->findNodes("Person");
	auto companyNodes = nodeLabelIndex->findNodes("Company");
	ASSERT_EQ(personNodes.size(), 2);
	EXPECT_NE(std::ranges::find(personNodes, 1), personNodes.end());
	EXPECT_NE(std::ranges::find(personNodes, 2), personNodes.end());
	ASSERT_EQ(companyNodes.size(), 1);
	EXPECT_EQ(companyNodes[0], 3);

	auto aliceNodes = nodePropertyIndex->findExactMatch("name", std::string("Alice"));
	auto ageNodes = nodePropertyIndex->findExactMatch("age", 30);
	ASSERT_EQ(aliceNodes.size(), 1);
	EXPECT_EQ(aliceNodes[0], 1);
	ASSERT_EQ(ageNodes.size(), 1);
	EXPECT_EQ(ageNodes[0], 1);

	// Assert: --- Verify Edge Indexes ---
	auto edgeLabelIndex = indexManager->getEdgeIndexManager()->getLabelIndex();
	auto edgePropertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();

	auto knowsEdges = edgeLabelIndex->findNodes("KNOWS"); // findNodes is generic
	auto worksAtEdges = edgeLabelIndex->findNodes("WORKS_AT");
	ASSERT_EQ(knowsEdges.size(), 1);
	EXPECT_EQ(knowsEdges[0], 1);
	ASSERT_EQ(worksAtEdges.size(), 1);
	EXPECT_EQ(worksAtEdges[0], 2);

	auto sinceEdges = edgePropertyIndex->findExactMatch("since", 2020);
	ASSERT_EQ(sinceEdges.size(), 1);
	EXPECT_EQ(sinceEdges[0], 1);

	// Assert: --- Verify Inactive Entities Are Not Indexed ---
	auto inactiveNodes = nodePropertyIndex->findExactMatch("name", std::string("InactiveUser"));
	EXPECT_TRUE(inactiveNodes.empty());
	EXPECT_EQ(std::ranges::find(personNodes, 4), personNodes.end());
	// Inactive edge ID was 3, its label was "KNOWS". The only active "KNOWS" edge is ID 1.
	EXPECT_EQ(std::ranges::find(knowsEdges, 3), knowsEdges.end());
}

// Test 3: Verify that building all node indexes creates BOTH label and property indexes for nodes, but leaves edge
// indexes untouched. This test replaces the old `BuildLabelIndex_Only` test, adapting its intent to the new API.
TEST_F(IndexBuilderTest, BuildAllNodeIndexes_VerifiesIsolation) {
	// Arrange
	populateDatabaseForTests();

	// Act
	EXPECT_TRUE(indexBuilder->buildAllNodeIndexes());

	// Assert: Verify node indexes are built
	auto nodeLabelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	EXPECT_FALSE(nodeLabelIndex->isEmpty());
	EXPECT_FALSE(nodePropertyIndex->isEmpty());
	ASSERT_EQ(nodeLabelIndex->findNodes("Person").size(), 2);
	ASSERT_EQ(nodePropertyIndex->findExactMatch("name", std::string("Alice")).size(), 1);

	// Assert: Verify edge indexes are NOT built
	auto edgeLabelIndex = indexManager->getEdgeIndexManager()->getLabelIndex();
	auto edgePropertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();
	EXPECT_TRUE(edgeLabelIndex->isEmpty());
	EXPECT_TRUE(edgePropertyIndex->isEmpty());
}


// Test 4: Build property index for a specific node key only
TEST_F(IndexBuilderTest, BuildNodePropertyIndex_SpecificKey) {
	// Arrange
	populateDatabaseForTests();

	// Act
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("name"));

	// Assert: Verify "name" property index is built
	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	auto aliceNodes = nodePropertyIndex->findExactMatch("name", std::string("Alice"));
	ASSERT_EQ(aliceNodes.size(), 1);
	EXPECT_EQ(aliceNodes[0], 1);
	EXPECT_TRUE(nodePropertyIndex->hasKeyIndexed("name"));

	// Assert: Verify other properties (like "age") are not indexed
	EXPECT_FALSE(nodePropertyIndex->hasKeyIndexed("age"));
	EXPECT_TRUE(nodePropertyIndex->findExactMatch("age", 30).empty());

	// Assert: Verify other indexes (node label, all edge indexes) are not built
	EXPECT_TRUE(indexManager->getNodeIndexManager()->getLabelIndex()->isEmpty());
	EXPECT_TRUE(indexManager->getEdgeIndexManager()->getLabelIndex()->isEmpty());
	EXPECT_TRUE(indexManager->getEdgeIndexManager()->getPropertyIndex()->isEmpty());
}

// Test 5: Verify correctness of ID range functions
// This test checks the internal logic of the builder. It's useful for debugging.
TEST_F(IndexBuilderTest, GetNodeAndEdgeIdRanges) {
	// Arrange (empty DB)
	EXPECT_TRUE(indexBuilder->getNodeIdRanges().empty());
	EXPECT_TRUE(indexBuilder->getEdgeIdRanges().empty());

	// Arrange (populated DB)
	populateDatabaseForTests();

	// Act
	auto nodeRanges = indexBuilder->getNodeIdRanges();
	auto edgeRanges = indexBuilder->getEdgeIdRanges();

	// Assert
	ASSERT_FALSE(nodeRanges.empty());
	// Node IDs are 1, 2, 3, 4
	EXPECT_LE(nodeRanges[0].first, 1);
	EXPECT_GE(nodeRanges.back().second, 4);

	ASSERT_FALSE(edgeRanges.empty());
	// Edge IDs are 1, 2
	EXPECT_LE(edgeRanges[0].first, 1);
	EXPECT_GE(edgeRanges.back().second, 2);
}

// Test 6: Test batching logic for node index building
TEST_F(IndexBuilderTest, BuildAllNodeIndexes_BatchingLogic) {
	constexpr int64_t numNodes = BATCH_SIZE + 5;

	// --- Phase 1: Arrange and Write Data ---
	{
		for (int i = 1; i <= numNodes; ++i) {
			graph::Node node(i, "TestNode");
			dataManager->addNode(node);
			dataManager->addNodeProperties(i, {{"test_id", static_cast<int64_t>(i)}});
		}
		// THE FIX: Close the database to force all writes to disk.
		close_database();
	}

	// --- Phase 2: Reopen, Act, and Assert ---
	{
		// Reopen the database to read the persisted state.
		open_database();

		// Act
		EXPECT_TRUE(indexBuilder->buildAllNodeIndexes());

		// Assert
		auto nodeLabelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
		auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();

		// Check the first node
		auto firstNodeResult = nodePropertyIndex->findExactMatch("test_id", static_cast<int64_t>(1));
		ASSERT_EQ(firstNodeResult.size(), 1);
		EXPECT_EQ(firstNodeResult[0], 1);

		// Check the last node (from the second batch)
		auto lastNodeResult = nodePropertyIndex->findExactMatch("test_id", static_cast<int64_t>(numNodes));
		ASSERT_EQ(lastNodeResult.size(), 1);
		EXPECT_EQ(lastNodeResult[0], numNodes);

		// Check the total count
		auto allTestNodes = nodeLabelIndex->findNodes("TestNode");
		EXPECT_EQ(allTestNodes.size(), numNodes);
	}
}

// Test that specifically tracks batch processing
TEST_F(IndexBuilderTest, BuildNodeIndexes_BatchProcessingAnalysis) {
    // Arrange - Use smaller batches for easier debugging
    constexpr int smallerBatchSize = 100;
    constexpr int numNodes = smallerBatchSize * 3 + 5; // Multiple batches with remainder

    for (int i = 1; i <= numNodes; ++i) {
        graph::Node node(i, "DebugTestNode");
        dataManager->addNode(node);
        dataManager->addNodeProperties(i, {{"batch_id", static_cast<int64_t>(i)}});
    }
    fileStorage->flush();

    // Add logging to examine node ranges
    auto nodeRanges = indexBuilder->getNodeIdRanges();
    std::cout << "Node ranges found: " << nodeRanges.size() << std::endl;
    for (const auto& [start, end] : nodeRanges) {
        std::cout << "Range: " << start << " to " << end << " (count: " << (end-start+1) << ")" << std::endl;
    }

    // Act
    EXPECT_TRUE(indexBuilder->buildAllNodeIndexes());

    // Assert - Check total count
    auto nodeLabelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
    auto allTestNodes = nodeLabelIndex->findNodes("DebugTestNode");

    // Log detailed results
    std::cout << "Expected nodes: " << numNodes << ", Found nodes: " << allTestNodes.size() << std::endl;

    EXPECT_EQ(allTestNodes.size(), numNodes);
}