/**
 * @file RelationshipTraversal.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <random>
#include "graph/traversal/RelationshipTraversal.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class RelationshipTraversalTest : public ::testing::Test {
protected:
    void SetUp() override {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        testFilePath = std::filesystem::temp_directory_path() / ("test_relationship_" + to_string(uuid) + ".dat");
        database = std::make_unique<graph::Database>(testFilePath.string());
        database->open();
        fileStorage = database->getStorage();
        dataManager = fileStorage->getDataManager();
        traversal = std::make_unique<graph::traversal::RelationshipTraversal>(dataManager);

        nodeA = graph::Node(1);
        nodeB = graph::Node(2);
        dataManager->addNodeEntity(nodeA);
        dataManager->addNodeEntity(nodeB);

        edge = graph::Edge(10, nodeA.getId(), nodeB.getId());
        edge.setActive(true);
        dataManager->addEdgeEntity(edge);
    }

    void TearDown() override {
        database->close();
        database.reset();
        std::filesystem::remove(testFilePath);
    }

    std::filesystem::path testFilePath;
    std::unique_ptr<graph::Database> database;
    std::shared_ptr<graph::storage::FileStorage> fileStorage;
    std::shared_ptr<graph::storage::DataManager> dataManager;
    std::unique_ptr<graph::traversal::RelationshipTraversal> traversal;
    graph::Node nodeA, nodeB;
    graph::Edge edge;
};

TEST_F(RelationshipTraversalTest, OutgoingAndIncomingEdges) {
    EXPECT_TRUE(traversal->getOutgoingEdges(nodeA.getId()).empty());
    EXPECT_TRUE(traversal->getIncomingEdges(nodeB.getId()).empty());

    traversal->linkEdge(edge);
    auto outEdges = traversal->getOutgoingEdges(nodeA.getId());
    auto inEdges = traversal->getIncomingEdges(nodeB.getId());
    ASSERT_EQ(outEdges.size(), 1);
    ASSERT_EQ(inEdges.size(), 1);
    EXPECT_EQ(outEdges[0].getId(), edge.getId());
    EXPECT_EQ(inEdges[0].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, AllConnectedEdges) {
    traversal->linkEdge(edge);
    auto allEdgesA = traversal->getAllConnectedEdges(nodeA.getId());
    auto allEdgesB = traversal->getAllConnectedEdges(nodeB.getId());
    EXPECT_EQ(allEdgesA.size(), 1);
    EXPECT_EQ(allEdgesB.size(), 1);
}

TEST_F(RelationshipTraversalTest, ConnectedNodes) {
    traversal->linkEdge(edge);
    auto targets = traversal->getConnectedTargetNodes(nodeA.getId());
    auto sources = traversal->getConnectedSourceNodes(nodeB.getId());
    ASSERT_EQ(targets.size(), 1);
    ASSERT_EQ(sources.size(), 1);
    EXPECT_EQ(targets[0].getId(), nodeB.getId());
    EXPECT_EQ(sources[0].getId(), nodeA.getId());
}

TEST_F(RelationshipTraversalTest, AllConnectedNodes) {
    traversal->linkEdge(edge);
    auto connectedA = traversal->getAllConnectedNodes(nodeA.getId());
    auto connectedB = traversal->getAllConnectedNodes(nodeB.getId());
    ASSERT_EQ(connectedA.size(), 1);
    ASSERT_EQ(connectedB.size(), 1);
    EXPECT_EQ(connectedA[0].getId(), nodeB.getId());
    EXPECT_EQ(connectedB[0].getId(), nodeA.getId());
}

TEST_F(RelationshipTraversalTest, LinkAndUnlinkEdge) {
    traversal->linkEdge(edge);
    traversal->unlinkEdge(edge);
    EXPECT_TRUE(traversal->getOutgoingEdges(nodeA.getId()).empty());
    EXPECT_TRUE(traversal->getIncomingEdges(nodeB.getId()).empty());

    EXPECT_EQ(edge.getNextOutEdgeId(), 0);
    EXPECT_EQ(edge.getPrevOutEdgeId(), 0);
    EXPECT_EQ(edge.getNextInEdgeId(), 0);
    EXPECT_EQ(edge.getPrevInEdgeId(), 0);
}