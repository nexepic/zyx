/**
 * @file test_IndexBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <chrono>
#include "graph/query/indexes/IndexBuilder.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"

class IndexBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        testFilePath = std::filesystem::temp_directory_path() / ("test_indexBuilder_" + to_string(uuid) + ".dat");
        database = std::make_unique<graph::Database>(testFilePath.string());
        database->open();
        fileStorage = database->getStorage();
        indexManager = std::make_shared<graph::query::indexes::IndexManager>(fileStorage);
        indexManager->initialize();
        indexBuilder = std::make_unique<graph::query::indexes::IndexBuilder>(indexManager, fileStorage);
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
    std::unique_ptr<graph::query::indexes::IndexBuilder> indexBuilder;
};

TEST_F(IndexBuilderTest, StartBuildAllIndexes) {
    EXPECT_TRUE(indexBuilder->startBuildAllIndexes());
    EXPECT_TRUE(indexBuilder->isBuilding() || indexBuilder->getStatus() != graph::query::indexes::IndexBuildStatus::FAILED);
    indexBuilder->waitForCompletion(std::chrono::seconds(5));
    EXPECT_EQ(indexBuilder->getStatus(), graph::query::indexes::IndexBuildStatus::COMPLETED);
    EXPECT_EQ(indexBuilder->getProgress(), 100);
}

TEST_F(IndexBuilderTest, StartBuildLabelIndex) {
    EXPECT_TRUE(indexBuilder->startBuildLabelIndex());
    indexBuilder->waitForCompletion(std::chrono::seconds(5));
    EXPECT_EQ(indexBuilder->getStatus(), graph::query::indexes::IndexBuildStatus::COMPLETED);
}

TEST_F(IndexBuilderTest, StartBuildPropertyIndex) {
    EXPECT_TRUE(indexBuilder->startBuildPropertyIndex("name"));
    indexBuilder->waitForCompletion(std::chrono::seconds(5));
    EXPECT_EQ(indexBuilder->getStatus(), graph::query::indexes::IndexBuildStatus::COMPLETED);
}

TEST_F(IndexBuilderTest, CancelBuild) {
    EXPECT_TRUE(indexBuilder->startBuildAllIndexes());
    indexBuilder->cancel();
    EXPECT_NE(indexBuilder->getStatus(), graph::query::indexes::IndexBuildStatus::COMPLETED);
}

TEST_F(IndexBuilderTest, DoubleStartReturnsFalse) {
    EXPECT_TRUE(indexBuilder->startBuildAllIndexes());
    EXPECT_FALSE(indexBuilder->startBuildLabelIndex());
    indexBuilder->cancel();
}