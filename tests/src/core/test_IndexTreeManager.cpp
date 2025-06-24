/**
 * @file test_IndexTreeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "TestTreeManagerConfig.hpp"

using namespace graph::query::indexes;
using namespace graph::storage::test;

class IndexTreeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestTreeManagerConfig::initialize();
        TestTreeManagerConfig::setMaxKeysPerNode(5); // Set MAX_KEYS_PER_NODE globally
        treeManager = TestTreeManagerConfig::getTreeManager();
        rootId = treeManager->initialize();
    }

    void TearDown() override {
        TestTreeManagerConfig::resetMaxKeysPerNode(); // Reset MAX_KEYS_PER_NODE
    }

    std::shared_ptr<IndexTreeManager> treeManager;
    int64_t rootId{};
};

TEST_F(IndexTreeManagerTest, InsertAndSplit) {
    for (int i = 0; i < 10; ++i) {
        rootId = treeManager->insert(rootId, "key" + std::to_string(i), i);
    }

    auto results = treeManager->findRange(rootId, "key0", "key9");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 10);
    ASSERT_EQ(results.front(), 0);
    ASSERT_EQ(results.back(), 9);
}

TEST_F(IndexTreeManagerTest, NodeMerge) {
    for (int i = 0; i < 5; ++i) {
        rootId = treeManager->insert(rootId, "key" + std::to_string(i), i);
    }

    treeManager->remove(rootId, "key0", 0);

    auto results = treeManager->findRange(rootId, "key1", "key4");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 4);
    ASSERT_EQ(results.front(), 1);
    ASSERT_EQ(results.back(), 4);
}

TEST_F(IndexTreeManagerTest, EmptyRangeQuery) {
    auto results = treeManager->findRange(rootId, "key0", "key9");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_TRUE(results.empty()); // Ensure no results are returned
}

TEST_F(IndexTreeManagerTest, SingleKeyRangeQuery) {
    treeManager->insert(rootId, "key5", 5);
    auto results = treeManager->findRange(rootId, "key5", "key5");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 1);
    ASSERT_EQ(results[0], 5);
}

TEST_F(IndexTreeManagerTest, BoundaryRangeQuery) {
    for (int i = 0; i < 10; ++i) {
        treeManager->insert(rootId, "key" + std::to_string(i), i);
    }
    auto results = treeManager->findRange(rootId, "key0", "key9");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 10);
    ASSERT_EQ(results.front(), 0);
    ASSERT_EQ(results.back(), 9);
}

TEST_F(IndexTreeManagerTest, DuplicateKeyRangeQuery) {
    treeManager->insert(rootId, "key5", 5);
    treeManager->insert(rootId, "key5", 6); // Duplicate key with different value
    auto results = treeManager->findRange(rootId, "key5", "key5");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0], 5);
    ASSERT_EQ(results[1], 6);
}

TEST_F(IndexTreeManagerTest, EdgeCaseEmptyNode) {
    auto results = treeManager->findRange(rootId, "key0", "key9");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_TRUE(results.empty()); // Ensure no results are returned for an empty node
}

TEST_F(IndexTreeManagerTest, EdgeCaseMaxKeys) {
    for (int i = 0; i < 5; ++i) {
        treeManager->insert(rootId, "key" + std::to_string(i), i);
    }

    auto results = treeManager->findRange(rootId, "key0", "key4");

    // Sort the results to ensure consistent order
    std::sort(results.begin(), results.end());

    ASSERT_EQ(results.size(), 5); // Ensure the node handles max keys correctly
}