/**
 * @file test_VectorIndexManager.cpp
 * @author Nexepic
 * @date 2026/1/26
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

#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

using namespace graph::query::indexes;

class ManagerPersistenceTest : public ::testing::Test {
protected:
	std::filesystem::path dbPath;

	void SetUp() override {
		// Deterministic path to allow reopening
		dbPath = std::filesystem::temp_directory_path() / "vim_persistence_test.db";
		std::filesystem::remove_all(dbPath); // Clean start
	}

	void TearDown() override {
		std::error_code ec;
		std::filesystem::remove_all(dbPath, ec);
	}
};

TEST_F(ManagerPersistenceTest, ReloadFromDisk) {
	// 1. Session A: Create Indexes
	{
		graph::Database db(dbPath.string());
		db.open();
		auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

		vim->createIndex("idx_l2", 4, "L2");
		vim->createIndex("idx_ip", 8, "IP"); // Hits metricType == 1 logic

		// Verify in-memory state
		EXPECT_TRUE(vim->hasIndex("idx_l2"));
	} // DB Closed

	// 2. Session B: Re-open DB
	{
		graph::Database db(dbPath.string());
		db.open();
		auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

		// 3. Test hasIndex (Disk Path)
		// Since it's a fresh VIM instance, indexes_ map is empty.
		// hasIndex("idx_l2") triggers the try-catch block reading from Registry
		EXPECT_TRUE(vim->hasIndex("idx_l2")); // Covers lines 87-91
		EXPECT_FALSE(vim->hasIndex("non_existent")); // Covers line 92

		// 4. Test getIndex (Disk Path)
		// Triggers loading from disk logic (lines 42-53)
		auto idxL2 = vim->getIndex("idx_l2");
		EXPECT_NE(idxL2, nullptr);
		// We can't easily check config metric string directly unless exposed,
		// but we can verify behavior if needed.

		auto idxIP = vim->getIndex("idx_ip");
		EXPECT_NE(idxIP, nullptr);
		// This covers the `if (cfg.metricType == 1)` branch
	}
}

TEST_F(ManagerPersistenceTest, CreateCosineIndex) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	// Cosine metric should be stored as metricType=1 (same as IP)
	vim->createIndex("idx_cosine", 4, "Cosine");
	EXPECT_TRUE(vim->hasIndex("idx_cosine"));

	auto idx = vim->getIndex("idx_cosine");
	EXPECT_NE(idx, nullptr);
}

TEST_F(ManagerPersistenceTest, HasIndex_NonExistent) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	EXPECT_FALSE(vim->hasIndex("non_existent_index"));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_WithVectorProperty) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto sm = db.getStorage()->getSystemStateManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	// 1. Create a vector index via IndexManager
	db.getQueryEngine()->getIndexManager()->createVectorIndex("emb_idx", "Person", "embedding", 4, "L2");

	// 2. Create a vector index instance
	vim->createIndex("emb_idx", 4, "L2");

	// 3. Create a node and call updateIndex
	graph::Node node(1, dm->getOrCreateLabelId("Person"));
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(1.0);
	embedding.emplace_back(0.0);
	embedding.emplace_back(0.0);
	embedding.emplace_back(0.0);
	props["embedding"] = graph::PropertyValue(embedding);

	EXPECT_NO_THROW(vim->updateIndex(node, "Person", props));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_EmptyLabel) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	graph::Node node(1, 0);
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["x"] = graph::PropertyValue(int64_t(1));

	// Empty label should be a no-op
	EXPECT_NO_THROW(vim->updateIndex(node, "", props));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_NonListProperty) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	// Create vector index
	db.getQueryEngine()->getIndexManager()->createVectorIndex("emb_idx2", "Person", "embedding", 4, "L2");
	vim->createIndex("emb_idx2", 4, "L2");

	graph::Node node(1, dm->getOrCreateLabelId("Person"));
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	// Non-list property for a vector-indexed field -> should log warning
	props["embedding"] = graph::PropertyValue(int64_t(42));

	EXPECT_NO_THROW(vim->updateIndex(node, "Person", props));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_IntegerVectorElements) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("emb_int", "Item", "vec", 4, "L2");
	vim->createIndex("emb_int", 4, "L2");

	graph::Node node(1, dm->getOrCreateLabelId("Item"));
	dm->addNode(node);

	// Use integer elements in the vector (covers INTEGER branch in updateIndex)
	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(int64_t(1));
	embedding.emplace_back(int64_t(0));
	embedding.emplace_back(int64_t(0));
	embedding.emplace_back(int64_t(0));
	props["vec"] = graph::PropertyValue(embedding);

	EXPECT_NO_THROW(vim->updateIndex(node, "Item", props));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_NonNumericVectorElement) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("emb_bad", "Thing", "vec", 3, "L2");
	vim->createIndex("emb_bad", 3, "L2");

	graph::Node node(1, dm->getOrCreateLabelId("Thing"));
	dm->addNode(node);

	// String element in vector -> should throw and be caught
	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(1.0);
	embedding.emplace_back(std::string("not_a_number"));
	embedding.emplace_back(0.0);
	props["vec"] = graph::PropertyValue(embedding);

	// Should not throw (error is caught internally and logged)
	EXPECT_NO_THROW(vim->updateIndex(node, "Thing", props));
}

TEST_F(ManagerPersistenceTest, RemoveIndex_Basic) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("rem_idx", "Person", "embedding", 4, "L2");
	vim->createIndex("rem_idx", 4, "L2");

	// Insert a vector
	graph::Node node(1, dm->getOrCreateLabelId("Person"));
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(1.0);
	embedding.emplace_back(0.0);
	embedding.emplace_back(0.0);
	embedding.emplace_back(0.0);
	props["embedding"] = graph::PropertyValue(embedding);
	vim->updateIndex(node, "Person", props);

	// Remove from index
	EXPECT_NO_THROW(vim->removeIndex(node.getId(), "Person"));
}

TEST_F(ManagerPersistenceTest, RemoveIndex_EmptyLabel) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	// Empty label should be handled gracefully
	EXPECT_NO_THROW(vim->removeIndex(1, "NonExistentLabel"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_Basic) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("batch_idx", "Person", "embedding", 4, "L2");
	vim->createIndex("batch_idx", 4, "L2");

	int64_t labelId = dm->getOrCreateLabelId("Person");

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;

	for (int i = 1; i <= 3; ++i) {
		graph::Node node(i, labelId);
		dm->addNode(node);

		std::unordered_map<std::string, graph::PropertyValue> props;
		std::vector<graph::PropertyValue> embedding;
		embedding.emplace_back(static_cast<double>(i));
		embedding.emplace_back(0.0);
		embedding.emplace_back(0.0);
		embedding.emplace_back(0.0);
		props["embedding"] = graph::PropertyValue(embedding);

		nodesWithProps.push_back({node, props});
	}

	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, "Person"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_EmptyLabel) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({graph::Node(1, 0), {}});

	// Empty label should be a no-op
	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, ""));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_EmptyNodes) {
	graph::Database db(dbPath.string());
	db.open();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> emptyNodes;

	// Empty nodes should be a no-op
	EXPECT_NO_THROW(vim->updateIndexBatch(emptyNodes, "Person"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_NonNumericElement) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("batch_bad", "Item", "vec", 3, "L2");
	vim->createIndex("batch_bad", 3, "L2");

	int64_t labelId = dm->getOrCreateLabelId("Item");
	graph::Node node(1, labelId);
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(1.0);
	embedding.emplace_back(std::string("bad"));
	embedding.emplace_back(0.0);
	props["vec"] = graph::PropertyValue(embedding);

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({node, props});

	// Should not throw (error is caught internally)
	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, "Item"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_NonListProperty) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("batch_nlist", "Item", "vec", 3, "L2");
	vim->createIndex("batch_nlist", 3, "L2");

	int64_t labelId = dm->getOrCreateLabelId("Item");
	graph::Node node(1, labelId);
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["vec"] = graph::PropertyValue(int64_t(42)); // not a LIST

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({node, props});

	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, "Item"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_IntegerElements) {
	graph::Database db(dbPath.string());
	db.open();
	auto dm = db.getStorage()->getDataManager();
	auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

	db.getQueryEngine()->getIndexManager()->createVectorIndex("batch_int", "Item", "vec", 4, "L2");
	vim->createIndex("batch_int", 4, "L2");

	int64_t labelId = dm->getOrCreateLabelId("Item");
	graph::Node node(1, labelId);
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(int64_t(1));
	embedding.emplace_back(int64_t(2));
	embedding.emplace_back(int64_t(3));
	embedding.emplace_back(int64_t(4));
	props["vec"] = graph::PropertyValue(embedding);

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({node, props});

	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, "Item"));
}