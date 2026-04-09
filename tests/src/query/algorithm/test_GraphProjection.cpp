/**
 * @file test_GraphProjection.cpp
 * @author Nexepic
 * @date 2026/4/9
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/GraphProjectionManager.hpp"

namespace fs = std::filesystem;

class GraphProjectionTest : public ::testing::Test {
protected:
	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_projection_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (database) database->close();
		database.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	void flushAndReopen() {
		database->getStorage()->flush();
		database->close();
		database->open();
		dataManager = database->getStorage()->getDataManager();
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
};

// ============================================================================
// GraphProjection::build Tests
// ============================================================================

TEST_F(GraphProjectionTest, BuildEmptyGraph) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 0u);
	EXPECT_EQ(proj.edgeCount(), 0u);
	EXPECT_FALSE(proj.isWeighted());
}

TEST_F(GraphProjectionTest, BuildSingleNode) {
	graph::Node n(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 1u);
	EXPECT_EQ(proj.edgeCount(), 0u);
	EXPECT_TRUE(proj.getNodeIds().contains(n.getId()));
}

TEST_F(GraphProjectionTest, BuildWithEdges) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	dataManager->addEdge(e);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_EQ(proj.edgeCount(), 1u);

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].targetId, n2.getId());
	EXPECT_DOUBLE_EQ(out[0].weight, 1.0);

	const auto &in = proj.getInNeighbors(n2.getId());
	ASSERT_EQ(in.size(), 1u);
	EXPECT_EQ(in[0].targetId, n1.getId());
}

TEST_F(GraphProjectionTest, BuildWithNodeLabelFilter) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n3(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	graph::Edge e1(0, n1.getId(), n3.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	graph::Edge e2(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("LIVES_IN"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	flushAndReopen();

	// Filter: only Person nodes
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Person");
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_FALSE(proj.getNodeIds().contains(n2.getId()));

	// Edge to n2 (City) should be excluded since n2 is not in projection
	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].targetId, n3.getId());
}

TEST_F(GraphProjectionTest, BuildWithEdgeLabelFilter) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e1(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("KNOWS"));
	graph::Edge e2(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("WORKS_WITH"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "KNOWS");
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_EQ(proj.edgeCount(), 1u);
}

TEST_F(GraphProjectionTest, BuildWithWeightProperty) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(e.getId(), {{"distance", graph::PropertyValue(42.5)}});
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "distance");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 42.5);
}

TEST_F(GraphProjectionTest, BuildWithMissingWeightPropertyFallsBackToOne) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	// No 'distance' property set
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "distance");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 1.0);
}

TEST_F(GraphProjectionTest, GetNeighborsNonExistentNode) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	const auto &out = proj.getOutNeighbors(999);
	EXPECT_TRUE(out.empty());
	const auto &in = proj.getInNeighbors(999);
	EXPECT_TRUE(in.empty());
}

TEST_F(GraphProjectionTest, BuildWithDeletedNodes) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("Person"));
	graph::Node n3(0, dataManager->getOrCreateTokenId("Person"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNode(n3);

	// Delete n2 to create a gap (covers !node.isActive() branch)
	dataManager->deleteNode(n2);
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	EXPECT_EQ(proj.nodeCount(), 2u);
	EXPECT_TRUE(proj.getNodeIds().contains(n1.getId()));
	EXPECT_FALSE(proj.getNodeIds().contains(n2.getId()));
	EXPECT_TRUE(proj.getNodeIds().contains(n3.getId()));
}

TEST_F(GraphProjectionTest, BuildWithIntegerWeightProperty) {
	graph::Node n1(0, dataManager->getOrCreateTokenId("City"));
	graph::Node n2(0, dataManager->getOrCreateTokenId("City"));
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), dataManager->getOrCreateTokenId("ROAD"));
	dataManager->addEdge(e);
	// Use INTEGER weight instead of DOUBLE (covers INTEGER branch in weight resolution)
	dataManager->addEdgeProperties(e.getId(), {{"hops", graph::PropertyValue(static_cast<int64_t>(7))}});
	flushAndReopen();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "", "", "hops");
	EXPECT_TRUE(proj.isWeighted());

	const auto &out = proj.getOutNeighbors(n1.getId());
	ASSERT_EQ(out.size(), 1u);
	EXPECT_DOUBLE_EQ(out[0].weight, 7.0);
}

// ============================================================================
// GraphProjectionManager Tests
// ============================================================================

TEST(GraphProjectionManagerTest, CreateAndGetProjection) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_TRUE(pm.exists("test"));
	auto retrieved = pm.getProjection("test");
	EXPECT_EQ(retrieved.get(), proj.get());
}

TEST(GraphProjectionManagerTest, DuplicateNameThrows) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_THROW(pm.createProjection("test", proj), std::runtime_error);
}

TEST(GraphProjectionManagerTest, GetNonExistentThrows) {
	graph::query::algorithm::GraphProjectionManager pm;
	EXPECT_THROW(pm.getProjection("missing"), std::runtime_error);
}

TEST(GraphProjectionManagerTest, DropProjection) {
	graph::query::algorithm::GraphProjectionManager pm;
	auto proj = std::make_shared<graph::query::algorithm::GraphProjection>();
	pm.createProjection("test", proj);

	EXPECT_TRUE(pm.dropProjection("test"));
	EXPECT_FALSE(pm.exists("test"));
	EXPECT_FALSE(pm.dropProjection("test")); // Already dropped
}
