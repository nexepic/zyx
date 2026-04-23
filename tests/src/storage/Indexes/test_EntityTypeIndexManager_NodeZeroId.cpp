/**
 * @file test_EntityTypeIndexManager_NodeZeroId.cpp
 * @brief Focused zero-id node branch tests for EntityTypeIndexManager.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace fs = std::filesystem;

class EntityTypeIndexManagerNodeZeroIdTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_entity_type_index_zero_id_" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		nodeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager,
				fileStorage->getSystemStateManager(),
				graph::query::indexes::IndexTypes::NODE_LABEL_TYPE,
				graph::storage::state::keys::Node::LABEL_ROOT,
				graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE,
				graph::storage::state::keys::Node::PROPERTY_PREFIX);
	}

	void TearDown() override {
		nodeIndexManager.reset();
		if (database) {
			database->close();
		}
		database.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath, ec);
		}
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> nodeIndexManager;
};

TEST_F(EntityTypeIndexManagerNodeZeroIdTest, OnEntityUpdated_NodeIdZeroSkipsMultiLabelDiff) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	const int64_t labelA = dataManager->getOrCreateTokenId("A");
	const int64_t labelB = dataManager->getOrCreateTokenId("B");
	graph::Node oldNode(1, labelA);
	graph::Node newNode(0, labelB); // id=0 drives the second condition false path

	nodeIndexManager->onEntityAdded(oldNode);
	auto labelIdx = nodeIndexManager->getLabelIndex();
	ASSERT_EQ(labelIdx->findNodes("A").size(), 1UL);

	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	// Update should skip node-label diff path for id=0 and leave existing index entry unchanged.
	EXPECT_EQ(labelIdx->findNodes("A").size(), 1UL);
	EXPECT_TRUE(labelIdx->findNodes("B").empty());
}

TEST_F(EntityTypeIndexManagerNodeZeroIdTest, OnEntityDeleted_NodeIdZeroSkipsDeleteDiff) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	const int64_t label = dataManager->getOrCreateTokenId("KeepMe");
	graph::Node indexedNode(42, label);
	nodeIndexManager->onEntityAdded(indexedNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	ASSERT_EQ(labelIdx->findNodes("KeepMe").size(), 1UL);

	graph::Node zeroIdNode(0, label);
	nodeIndexManager->onEntityDeleted(zeroIdNode);

	// Deleting id=0 should skip the multi-label delete loop.
	EXPECT_EQ(labelIdx->findNodes("KeepMe").size(), 1UL);
}
