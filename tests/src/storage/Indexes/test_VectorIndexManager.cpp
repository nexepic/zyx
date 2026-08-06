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
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

using namespace graph::query::indexes;

class ManagerPersistenceTest : public ::testing::Test {
protected:
	std::filesystem::path dbPath;

	void SetUp() override {
		// Deterministic path to allow reopening
		dbPath = std::filesystem::temp_directory_path() / "vim_persistence_test.zyx";
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
		auto storage = std::make_shared<graph::storage::FileStorage>(
				dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		storage->open();
		auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		indexManager->initialize();
		auto vim = indexManager->getVectorIndexManager();

		vim->createIndex("idx_l2", 4, "L2");
		vim->createIndex("idx_ip", 8, "IP"); // Hits metricType == 1 logic

		// Verify in-memory state
		EXPECT_TRUE(vim->hasIndex("idx_l2"));
		// The storage listener is weak. Persist while IndexManager still owns the
		// vector manager instead of relying on destruction order at scope exit.
		ASSERT_NO_THROW(storage->flushOrThrow());
	} // DB Closed

	// 2. Session B: Re-open DB
	{
		auto storage = std::make_shared<graph::storage::FileStorage>(
				dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
		storage->open();
		auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		indexManager->initialize();
		auto vim = indexManager->getVectorIndexManager();

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
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	// Cosine metric should be stored as metricType=1 (same as IP)
	vim->createIndex("idx_cosine", 4, "Cosine");
	EXPECT_TRUE(vim->hasIndex("idx_cosine"));

	auto idx = vim->getIndex("idx_cosine");
	EXPECT_NE(idx, nullptr);
}

TEST_F(ManagerPersistenceTest, DirectManagerWorksWithoutThreadPoolAndSkipsNonVectorMetadata) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto sm = storage->getSystemStateManager();
	auto im = indexManager;

	VectorIndexManager direct(dm, sm);
	direct.createIndex("direct_l2", 4, "L2");
	EXPECT_TRUE(direct.hasIndex("direct_l2"));
	EXPECT_NE(direct.getIndex("direct_l2"), nullptr);

	ASSERT_TRUE(im->createIndex("scalar_person_age", "node", "Person", "age"));
	graph::Node node(1, dm->getOrCreateTokenId("Person"));
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["age"] = graph::PropertyValue(int64_t(42));
	EXPECT_NO_THROW(direct.updateIndex(node, "Person", props));
}

TEST_F(ManagerPersistenceTest, RemoveIndexIgnoresIndexesForOtherLabels) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto im = indexManager;
	auto vim = im->getVectorIndexManager();

	im->createVectorIndex("person_vec_remove_skip", "Person", "embedding", 4, "L2");
	vim->createIndex("person_vec_remove_skip", 4, "L2");

	graph::Node node(1, dm->getOrCreateTokenId("Animal"));
	EXPECT_NO_THROW(vim->removeIndex(node.getId(), "Animal"));
}

TEST_F(ManagerPersistenceTest, HasIndex_NonExistent) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	EXPECT_FALSE(vim->hasIndex("non_existent_index"));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_WithVectorProperty) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto sm = storage->getSystemStateManager();
	auto vim = indexManager->getVectorIndexManager();

	// 1. Create a vector index via IndexManager
	indexManager->createVectorIndex("emb_idx", "Person", "embedding", 4, "L2");

	// 2. Create a vector index instance
	vim->createIndex("emb_idx", 4, "L2");

	// 3. Create a node and call updateIndex
	graph::Node node(1, dm->getOrCreateTokenId("Person"));
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
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	graph::Node node(1, 0);
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["x"] = graph::PropertyValue(int64_t(1));

	// Empty label should be a no-op
	EXPECT_NO_THROW(vim->updateIndex(node, "", props));
}

TEST_F(ManagerPersistenceTest, UpdateIndex_NonListProperty) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	// Create vector index
	indexManager->createVectorIndex("emb_idx2", "Person", "embedding", 4, "L2");
	vim->createIndex("emb_idx2", 4, "L2");

	graph::Node node(1, dm->getOrCreateTokenId("Person"));
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	// An indexed vector property with an incompatible type must abort the write.
	props["embedding"] = graph::PropertyValue(int64_t(42));

	EXPECT_THROW(vim->updateIndex(node, "Person", props), std::invalid_argument);
}

TEST_F(ManagerPersistenceTest, UpdateIndex_IntegerVectorElements) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("emb_int", "Item", "vec", 4, "L2");
	vim->createIndex("emb_int", 4, "L2");

	graph::Node node(1, dm->getOrCreateTokenId("Item"));
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

TEST_F(ManagerPersistenceTest, UpdateIndexSameVectorIsIdempotent) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("emb_idempotent", "Item", "vec", 3, "L2");
	vim->createIndex("emb_idempotent", 3, "L2");

	graph::Node node(1, dm->getOrCreateTokenId("Item"));
	dm->addNode(node);
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["vec"] = graph::PropertyValue(std::vector<graph::PropertyValue>{1.0, 2.0, 3.0});

	vim->updateIndex(node, "Item", props);
	auto index = vim->getIndex("emb_idempotent");
	ASSERT_EQ(index->size(), 1U);

	vim->updateIndex(node, "Item", props);
	EXPECT_EQ(index->size(), 1U);
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatchCoalescesDuplicateNodeIds) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("emb_batch_dedup", "Item", "vec", 3, "L2");
	vim->createIndex("emb_batch_dedup", 3, "L2");
	graph::Node node(1, dm->getOrCreateTokenId("Item"));
	dm->addNode(node);

	const auto makeProperties = [](std::vector<graph::PropertyValue> values) {
		return std::unordered_map<std::string, graph::PropertyValue>{
				{"vec", graph::PropertyValue(std::move(values))},
		};
	};
	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> batch;
	batch.push_back({node, makeProperties({1.0, 2.0, 3.0})});
	batch.push_back({node, makeProperties({4.0, 5.0, 6.0})});

	vim->updateIndexBatch(batch, "Item");
	EXPECT_EQ(vim->getIndex("emb_batch_dedup")->size(), 1U);
}

TEST_F(ManagerPersistenceTest, UpdateIndex_NonNumericVectorElement) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("emb_bad", "Thing", "vec", 3, "L2");
	vim->createIndex("emb_bad", 3, "L2");

	graph::Node node(1, dm->getOrCreateTokenId("Thing"));
	dm->addNode(node);

	// Invalid vector elements must propagate instead of leaving a stale index.
	std::unordered_map<std::string, graph::PropertyValue> props;
	std::vector<graph::PropertyValue> embedding;
	embedding.emplace_back(1.0);
	embedding.emplace_back(std::string("not_a_number"));
	embedding.emplace_back(0.0);
	props["vec"] = graph::PropertyValue(embedding);

	EXPECT_THROW(vim->updateIndex(node, "Thing", props), std::invalid_argument);
}

TEST_F(ManagerPersistenceTest, RemoveIndex_Basic) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("rem_idx", "Person", "embedding", 4, "L2");
	vim->createIndex("rem_idx", 4, "L2");

	// Insert a vector
	graph::Node node(1, dm->getOrCreateTokenId("Person"));
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
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	// Empty label should be handled gracefully
	EXPECT_NO_THROW(vim->removeIndex(1, "NonExistentLabel"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_Basic) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("batch_idx", "Person", "embedding", 4, "L2");
	vim->createIndex("batch_idx", 4, "L2");

	int64_t labelId = dm->getOrCreateTokenId("Person");

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
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({graph::Node(1, 0), {}});

	// Empty label should be a no-op
	EXPECT_NO_THROW(vim->updateIndexBatch(nodesWithProps, ""));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_EmptyNodes) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto vim = indexManager->getVectorIndexManager();

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> emptyNodes;

	// Empty nodes should be a no-op
	EXPECT_NO_THROW(vim->updateIndexBatch(emptyNodes, "Person"));
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_NonNumericElement) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("batch_bad", "Item", "vec", 3, "L2");
	vim->createIndex("batch_bad", 3, "L2");

	int64_t labelId = dm->getOrCreateTokenId("Item");
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

	EXPECT_THROW(vim->updateIndexBatch(nodesWithProps, "Item"), std::invalid_argument);
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_NonListProperty) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("batch_nlist", "Item", "vec", 3, "L2");
	vim->createIndex("batch_nlist", 3, "L2");

	int64_t labelId = dm->getOrCreateTokenId("Item");
	graph::Node node(1, labelId);
	dm->addNode(node);

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["vec"] = graph::PropertyValue(int64_t(42)); // not a LIST

	std::vector<std::pair<graph::Node, std::unordered_map<std::string, graph::PropertyValue>>> nodesWithProps;
	nodesWithProps.push_back({node, props});

	EXPECT_THROW(vim->updateIndexBatch(nodesWithProps, "Item"), std::invalid_argument);
}

TEST_F(ManagerPersistenceTest, UpdateIndexBatch_IntegerElements) {
	auto storage = std::make_shared<graph::storage::FileStorage>(
			dbPath.string(), 10'000U, graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	storage->open();
	auto indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
	indexManager->initialize();
	auto dm = storage->getDataManager();
	auto vim = indexManager->getVectorIndexManager();

	indexManager->createVectorIndex("batch_int", "Item", "vec", 4, "L2");
	vim->createIndex("batch_int", 4, "L2");

	int64_t labelId = dm->getOrCreateTokenId("Item");
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
