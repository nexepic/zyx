/**
 * @file test_IndexTreeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/storage/data/BlobManager.hpp"

using namespace graph;
using namespace graph::storage;
using namespace graph::query::indexes;

class IndexTreeManagerTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = std::filesystem::temp_directory_path() /
					  ("test_indexTreeManager_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<Database>(testDbPath_.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		if (database_) {
			database_->close();
			database_.reset();
			if (std::filesystem::exists(testDbPath_)) {
				std::filesystem::remove(testDbPath_);
			}
		}
	}

	void SetUp() override {
		// Create managers for specific key types as needed by tests.
		stringTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::STRING);
		stringRootId_ = stringTreeManager_->initialize();

		intTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::INTEGER);
		intRootId_ = intTreeManager_->initialize();
	}

	void TearDown() override {
		if (stringRootId_ != 0)
			stringTreeManager_->clear(stringRootId_);
		if (intRootId_ != 0)
			intTreeManager_->clear(intRootId_);
	}

	static std::string generatePaddedKey(int i, int width = 5) {
		std::ostringstream ss;
		ss << "key_" << std::setw(width) << std::setfill('0') << i;
		return ss.str();
	}

	static std::unique_ptr<Database> database_;
	static std::shared_ptr<DataManager> dataManager_;
	static std::filesystem::path testDbPath_;

	std::shared_ptr<IndexTreeManager> stringTreeManager_;
	int64_t stringRootId_{};
	std::shared_ptr<IndexTreeManager> intTreeManager_;
	int64_t intRootId_{};
};

std::unique_ptr<Database> IndexTreeManagerTest::database_ = nullptr;
std::shared_ptr<DataManager> IndexTreeManagerTest::dataManager_ = nullptr;
std::filesystem::path IndexTreeManagerTest::testDbPath_;


// --- Core B+Tree Functionality Tests ---

TEST_F(IndexTreeManagerTest, OperationsOnEmptyTree) {
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("any_key")).empty());
	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("any_key"), 123));

	auto range_results = stringTreeManager_->findRange(stringRootId_, PropertyValue("A"), PropertyValue("Z"));
	ASSERT_TRUE(range_results.empty());
}

TEST_F(IndexTreeManagerTest, InsertDuplicateAndMultipleValues) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 100);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 200);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 300);
	stringRootId_ =
			stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 200); // Duplicate should be ignored.

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("multi_key"));
	ASSERT_EQ(results.size(), 3);
	std::sort(results.begin(), results.end());
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results[1], 200);
	ASSERT_EQ(results[2], 300);
}

TEST_F(IndexTreeManagerTest, AdvancedRemoveScenarios) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 1);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 2);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 3);

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 99));
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).size(), 3);

	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 2));
	auto results1 = stringTreeManager_->find(stringRootId_, PropertyValue("test_key"));
	ASSERT_EQ(results1.size(), 2);
	ASSERT_EQ(std::find(results1.begin(), results1.end(), 2), results1.end());

	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 1));
	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 3));
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).empty());

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("non_existent_key"), 1));
}

// --- B+Tree Structural Integrity and Split Tests ---

TEST_F(IndexTreeManagerTest, InsertCausesLeafSplit) {
	auto rootNode = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(rootNode.isLeaf());

	int insertedCount = 0;
	for (int i = 0; i < 500; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key_" + std::to_string(i)), i);
		insertedCount++;
		rootNode = dataManager_->getIndex(stringRootId_);
		if (!rootNode.isLeaf())
			break;
	}

	ASSERT_FALSE(rootNode.isLeaf()) << "A leaf split should have turned the root into an internal node.";
	ASSERT_EQ(rootNode.getChildCount(), 2);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("key_0"));
	ASSERT_EQ(results[0], 0);
}

TEST_F(IndexTreeManagerTest, LeafNodeLinkedListIsCorrectAfterSplit) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("C"), 3);
	for (int i = 0; i < 500; ++i) { // Force a split
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	int64_t leaf1_id = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue("C"));
	ASSERT_NE(leaf1_id, 0);
	auto leaf1 = dataManager_->getIndex(leaf1_id);

	int64_t leaf2_id = leaf1.getNextLeafId();
	ASSERT_NE(leaf2_id, 0);
	auto leaf2 = dataManager_->getIndex(leaf2_id);
	ASSERT_EQ(leaf2.getPrevLeafId(), leaf1_id);
}

TEST_F(IndexTreeManagerTest, InsertCausesInternalSplit) {
	auto rootNode = dataManager_->getIndex(stringRootId_);
	uint8_t initialLevel = rootNode.getLevel();

	int insertedCount = 0;
	for (int i = 0; i < 10000; ++i) { // Need many insertions to split internal nodes
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
		insertedCount++;
		rootNode = dataManager_->getIndex(stringRootId_);
		if (rootNode.getLevel() > initialLevel + 1)
			break; // Height increased by 2 (e.g., L0 -> L2)
	}

	ASSERT_GT(rootNode.getLevel(), initialLevel + 1)
			<< "An internal root split should increase the tree height by at least 2 levels.";

	auto results = stringTreeManager_->findRange(stringRootId_, PropertyValue(generatePaddedKey(0)),
												 PropertyValue(generatePaddedKey(insertedCount - 1)));
	ASSERT_EQ(results.size(), insertedCount);
}

TEST_F(IndexTreeManagerTest, RangeQuerySpansMultipleLeaves) {
	for (int i = 0; i < 600; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	auto results = stringTreeManager_->findRange(stringRootId_, PropertyValue(generatePaddedKey(100)),
												 PropertyValue(generatePaddedKey(499)));
	ASSERT_EQ(results.size(), 400);
	std::sort(results.begin(), results.end());
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results.back(), 499);
}

// --- End-to-End Blob Storage Tests ---

TEST_F(IndexTreeManagerTest, LeafValuesOverflowToBlob_EndToEnd) {
	const std::string testKey = "key_with_many_values";
	size_t valueCount = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 5;

	for (size_t i = 0; i < valueCount; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(testKey), static_cast<int64_t>(i));
	}

	int64_t leafId = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue(testKey));
	ASSERT_NE(leafId, 0);
	auto leafNode = dataManager_->getIndex(leafId);
	auto entries = leafNode.getAllEntries(dataManager_);
	ASSERT_EQ(entries.size(), 1);
	ASSERT_NE(entries[0].valuesBlobId, 0) << "The entry's value list should have been offloaded to a blob.";

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(testKey));
	ASSERT_EQ(results.size(), valueCount);
}

TEST_F(IndexTreeManagerTest, InternalKeyPromotionToBlob_EndToEnd) {
	// This test ensures that all promoted keys are long enough to force Blob storage.
	const std::string suffix(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	int splitTriggerCount =
			500; // A number of insertions sufficient to cause multiple splits (including internal node splits).

	// Insert a large number of long keys to force multiple splits in the tree.
	for (int i = 0; i < splitTriggerCount; ++i) {
		// Make each key a long key.
		std::string currentLongKey = generatePaddedKey(i) + suffix;
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(currentLongKey), i);
	}

	// At this point, at least one promoted key in the tree must have been stored in a Blob.
	// Traversal and checking logic remain unchanged.
	bool foundBlobKeyInTree = false;
	std::vector<int64_t> nodesToVisit = {stringRootId_};
	std::set<int64_t> visited;

	while (!nodesToVisit.empty()) {
		int64_t currentId = nodesToVisit.back();
		nodesToVisit.pop_back();
		if (visited.count(currentId))
			continue;
		visited.insert(currentId);

		auto currentNode = dataManager_->getIndex(currentId);
		if (!currentNode.isLeaf()) {
			auto children = currentNode.getAllChildren(dataManager_);
			for (const auto &child: children) {
				if (child.keyBlobId != 0) {
					foundBlobKeyInTree = true;
					break;
				}
				nodesToVisit.push_back(child.childId);
			}
		}
		if (foundBlobKeyInTree)
			break;
	}

	ASSERT_TRUE(foundBlobKeyInTree) << "After multiple splits, no keys stored in Blob were found in internal nodes.";

	// Finally, verify that we can still find the correct value using this long key.
	std::string finalKeyToFind = generatePaddedKey(splitTriggerCount - 1) + suffix;
	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(finalKeyToFind));
	ASSERT_EQ(results.size(), 1);
	ASSERT_EQ(results[0], splitTriggerCount - 1);
}
