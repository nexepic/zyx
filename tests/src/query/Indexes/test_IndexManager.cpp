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
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Database.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class IndexManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_indexManager_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(fileStorage);
		indexManager->initialize();

		graph::Node node1(1, "label_1");
		dataManager->addNode(node1);
		dataManager->addNodeProperties(1, {{"name", "Node1"}, {"value", static_cast<int64_t>(42)}});
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
	std::shared_ptr<graph::storage::DataManager> dataManager;
};

TEST_F(IndexManagerTest, BuildAndDropLabelIndex) {
	EXPECT_TRUE(indexManager->buildLabelIndex());
	auto indexes = indexManager->listIndexes();
	ASSERT_FALSE(indexes.empty());
	EXPECT_EQ(indexes[0].first, "label");
	EXPECT_TRUE(indexManager->dropIndex("label", ""));
	EXPECT_TRUE(indexManager->listIndexes().empty());
}

TEST_F(IndexManagerTest, BuildAndDropPropertyIndex) {
	std::string key = "name";
	EXPECT_TRUE(indexManager->buildPropertyIndex(key));
	auto indexes = indexManager->listIndexes();
	bool found = false;
	for (const auto &idx: indexes) {
		if (idx.first == "property" && idx.second == key)
			found = true;
	}
	EXPECT_TRUE(found);
	EXPECT_TRUE(indexManager->dropIndex("property", key));
}

TEST_F(IndexManagerTest, BuildAllIndexesAndDropAll) {
	EXPECT_TRUE(indexManager->buildIndexes());
	auto indexes = indexManager->listIndexes();
	EXPECT_FALSE(indexes.empty());
	EXPECT_TRUE(indexManager->dropIndex("property", ""));
	EXPECT_TRUE(indexManager->dropIndex("label", ""));
}

TEST_F(IndexManagerTest, EnableAndDisableIndexes) {
	indexManager->enableLabelIndex(true);
	indexManager->enableLabelIndex(false);
	indexManager->enablePropertyIndex("foo", true);
	indexManager->enablePropertyIndex("foo", false);
	indexManager->enableRelationshipIndex(true);
	indexManager->enableRelationshipIndex(false);
	indexManager->enableFullTextIndex(true);
	indexManager->enableFullTextIndex(false);
}

TEST_F(IndexManagerTest, PersistStateNoThrow) { EXPECT_NO_THROW(indexManager->persistState()); }
