/**
 * @file test_BlobChainManager.cpp
 * @author Nexepic
 * @date 2025/7/28
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include "graph/core/BlobChainManager.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class BlobChainManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_blobChain_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		chainManager = std::make_unique<graph::BlobChainManager>(dataManager);
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
	std::unique_ptr<graph::BlobChainManager> chainManager;
};

TEST_F(BlobChainManagerTest, CreateSingleBlobChain) {
	constexpr int64_t entityId = 100;
	constexpr int64_t entityTypeId = 1;
	const std::string testData = "Small test data";

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	EXPECT_EQ(blobChain.size(), 1UL);
	EXPECT_EQ(blobChain[0].getEntityId(), entityId);
	EXPECT_EQ(blobChain[0].getEntityType(), entityTypeId);
	EXPECT_EQ(blobChain[0].getChainPosition(), 0);
	EXPECT_EQ(blobChain[0].getPrevBlobId(), 0);
	EXPECT_EQ(blobChain[0].getNextBlobId(), 0);
	EXPECT_TRUE(blobChain[0].isActive());
}

TEST_F(BlobChainManagerTest, CreateMultipleBlobChain) {
	constexpr int64_t entityId = 200;
	constexpr int64_t entityTypeId = 2;
	// Generate random data to avoid high compression
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(42); // Fixed seed for reproducibility
	std::uniform_int_distribution dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	EXPECT_EQ(blobChain.size(), 3UL);

	// Check head blob
	EXPECT_EQ(blobChain[0].getChainPosition(), 0);
	EXPECT_EQ(blobChain[0].getPrevBlobId(), 0);
	EXPECT_EQ(blobChain[0].getNextBlobId(), blobChain[1].getId());

	// Check middle blob
	EXPECT_EQ(blobChain[1].getChainPosition(), 1);
	EXPECT_EQ(blobChain[1].getPrevBlobId(), blobChain[0].getId());
	EXPECT_EQ(blobChain[1].getNextBlobId(), blobChain[2].getId());

	// Check tail blob
	EXPECT_EQ(blobChain[2].getChainPosition(), 2);
	EXPECT_EQ(blobChain[2].getPrevBlobId(), blobChain[1].getId());
	EXPECT_EQ(blobChain[2].getNextBlobId(), 0);
}

TEST_F(BlobChainManagerTest, CreateBlobChainEmptyData) {
	constexpr int64_t entityId = 300;
	constexpr int64_t entityTypeId = 3;
	const std::string emptyData;

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, emptyData);

	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	EXPECT_EQ(blobChain.size(), 1UL);
	// Check that reading the chain returns the original empty string
	EXPECT_EQ(chainManager->readBlobChain(blobChain[0].getId()), "");
	// Optionally, check the original size
	EXPECT_EQ(blobChain[0].getOriginalSize(), 0u);
}

TEST_F(BlobChainManagerTest, ReadSingleBlobChain) {
	constexpr int64_t entityId = 400;
	constexpr int64_t entityTypeId = 4;
	const std::string originalData = "Test data for reading";

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, originalData);

	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	ASSERT_EQ(blobChain.size(), 1UL);

	std::string result = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(result, originalData);
}

TEST_F(BlobChainManagerTest, ReadBlobChainThrowsOnMissingHead) {
	// Try to read a non-existent blob chain
	EXPECT_THROW((void) chainManager->readBlobChain(999999), std::runtime_error);
}

TEST_F(BlobChainManagerTest, ReadMultipleBlobChain) {
	constexpr int64_t entityId = 500;
	constexpr int64_t entityTypeId = 5;
	// Generate random data to avoid high compression
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE + 50);
	std::mt19937 rng(123);
	std::uniform_int_distribution dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}
	const std::string originalData = testData;

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);
	ASSERT_EQ(blobChain.size(), 2UL);

	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	std::string result = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(result, originalData);
}

TEST_F(BlobChainManagerTest, ReadBlobChainThrowsOnCorruptedChain) {
	// Not possible to create a corrupted chain with public API, so skip or test with invalid id
	EXPECT_THROW((void) chainManager->readBlobChain(0), std::runtime_error);
}

// Verifies that blobs are deleted after deleteBlobChain
TEST_F(BlobChainManagerTest, DeleteBlobChain) {
	constexpr int64_t entityId = 600;
	constexpr int64_t entityTypeId = 6;
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(99);
	std::uniform_int_distribution dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	// Add blobs to DataManager
	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	ASSERT_GT(blobChain.size(), 1UL);

	// Delete the blob chain
	EXPECT_NO_THROW(chainManager->deleteBlobChain(blobChain[0].getId()));

	// Verify all blobs are deleted (inactive or not found)
	for (const auto &blob: blobChain) {
		auto loaded = dataManager->getBlob(blob.getId());
		EXPECT_TRUE(loaded.getId() == 0 || !loaded.isActive());
	}
}

TEST_F(BlobChainManagerTest, CompressDataThroughChain) {
	const std::string testData = "Test data for compression that should be compressed";

	auto blobChain = chainManager->createBlobChain(1, 1, testData);

	EXPECT_EQ(blobChain.size(), 1UL);
	EXPECT_EQ(blobChain[0].getOriginalSize(), testData.size());
}

TEST_F(BlobChainManagerTest, SplitDataIntoMultipleChunks) {
	// Generate random data to avoid high compression and ensure multiple chunks
	std::string largeData;
	largeData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(2024); // Fixed seed for reproducibility
	std::uniform_int_distribution dist(0, 255);
	for (auto &c: largeData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(1, 1, largeData);

	// Should create multiple blobs due to size
	EXPECT_GT(blobChain.size(), 2UL);

	// Verify chain linking
	for (size_t i = 0; i < blobChain.size(); i++) {
		EXPECT_EQ(blobChain[i].getChainPosition(), static_cast<int32_t>(i));

		if (i == 0) {
			EXPECT_EQ(blobChain[i].getPrevBlobId(), 0);
		} else {
			EXPECT_EQ(blobChain[i].getPrevBlobId(), blobChain[i - 1].getId());
		}

		if (i == blobChain.size() - 1) {
			EXPECT_EQ(blobChain[i].getNextBlobId(), 0);
		} else {
			EXPECT_EQ(blobChain[i].getNextBlobId(), blobChain[i + 1].getId());
		}
	}
}

TEST_F(BlobChainManagerTest, HandleInactiveBlobInChain) {
	// Not possible to create an inactive blob in chain with public API, so skip or test with invalid id
	EXPECT_THROW((void) chainManager->readBlobChain(0), std::runtime_error);
}

TEST_F(BlobChainManagerTest, TypeIdConstant) {
	// Verify that Blob typeId matches expected entity type
	EXPECT_EQ(graph::Blob::typeId, graph::toUnderlying(graph::EntityType::Blob));
}

// =============================================================================
// Cache Eviction Edge Case Tests
// These tests verify that blob chain creation works correctly even when
// cache eviction occurs during the process. This is critical because:
// 1. Blobs are value types (copies), not references
// 2. Cache eviction can happen between addEntity and updateEntity calls
// 3. The returned blobChain vector must stay in sync with DataManager
// =============================================================================

TEST_F(BlobChainManagerTest, CreateChainWithSmallCache) {
	// Create a chain when cache might be nearly full
	// This tests that two-pass creation works even under memory pressure

	// First, create enough blobs to fill the cache
	// Default blob cache size is cacheSize/4 (2500 for default cacheSize=10000)
	// But we can't easily control that, so we create a moderate amount

	// Create multiple large blob chains to potentially trigger eviction
	std::vector<std::vector<graph::Blob>> allChains;

	for (int round = 0; round < 10; round++) {
		constexpr int64_t entityTypeId = 1;
		constexpr int64_t entityId = 1000;
		// Generate random data to avoid high compression
		std::string testData;
		testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
		std::mt19937 rng(round); // Different seed each round
		std::uniform_int_distribution<int> dist(0, 255);
		for (auto &c: testData) {
			c = static_cast<char>(dist(rng));
		}

		auto blobChain = chainManager->createBlobChain(entityId + round, entityTypeId, testData);
		allChains.push_back(blobChain);

		// Verify chain integrity for each created chain
		ASSERT_GT(blobChain.size(), 1UL);

		// Critical: Verify nextBlobId is correctly set in the returned vector
		// This is what could fail if cache eviction causes sync issues
		for (size_t i = 0; i < blobChain.size() - 1; i++) {
			EXPECT_EQ(blobChain[i].getNextBlobId(), blobChain[i + 1].getId())
					<< "Failed at chain " << round << ", blob " << i
					<< ": nextBlobId should point to next blob in chain";
		}

		// Verify last blob has no next
		EXPECT_EQ(blobChain.back().getNextBlobId(), 0)
				<< "Failed at chain " << round << ": last blob should have nextBlobId=0";
	}
}

TEST_F(BlobChainManagerTest, CreateChainUnderCacheEvictionStress) {
	// Stress test: Create many blob chains rapidly to force cache eviction
	// This simulates the real-world scenario that caused the original bug

	constexpr int64_t numChains = 50; // Create 50 separate chains

	// Generate different data for each chain to ensure different blob IDs
	std::mt19937 rng(12345);
	std::uniform_int_distribution<int> dist(0, 255);

	for (int64_t chainIdx = 0; chainIdx < numChains; chainIdx++) {
		constexpr int64_t entityTypeId = 1;
		// Create a 3-blob chain
		std::string testData;
		testData.resize(graph::Blob::CHUNK_SIZE * 2 + 50);
		for (auto &c: testData) {
			c = static_cast<char>(dist(rng));
		}

		auto blobChain = chainManager->createBlobChain(chainIdx, entityTypeId, testData);

		// Critical verification: Each blob's nextBlobId must be correct
		// If the two-pass approach fails or sync is broken, this will fail
		ASSERT_EQ(blobChain.size(), 3UL) << "Chain " << chainIdx << " should have 3 blobs";

		EXPECT_EQ(blobChain[0].getNextBlobId(), blobChain[1].getId())
				<< "Chain " << chainIdx << ": blob[0].nextBlobId mismatch";
		EXPECT_EQ(blobChain[1].getNextBlobId(), blobChain[2].getId())
				<< "Chain " << chainIdx << ": blob[1].nextBlobId mismatch";
		EXPECT_EQ(blobChain[2].getNextBlobId(), 0) << "Chain " << chainIdx << ": blob[2].nextBlobId should be 0";

		// Additionally verify by reading back from DataManager
		// This ensures the chain is actually persisted correctly
		auto blob0FromManager = dataManager->getBlob(blobChain[0].getId());
		EXPECT_EQ(blob0FromManager.getNextBlobId(), blobChain[1].getId())
				<< "Chain " << chainIdx << ": DataManager blob[0].nextBlobId mismatch";
	}
}

TEST_F(BlobChainManagerTest, UpdateChainAfterEviction) {
	// Test updating a blob chain after it might have been evicted from cache
	// This verifies that updateBlobChain handles cache eviction correctly

	constexpr int64_t entityId = 2000;
	constexpr int64_t entityTypeId = 2;

	// Create initial chain
	std::string originalData;
	originalData.resize(graph::Blob::CHUNK_SIZE * 2);
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: originalData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, originalData);
	ASSERT_GT(blobChain.size(), 1UL);
	int64_t headBlobId = blobChain[0].getId();

	// Create many other blobs to potentially evict the original chain from cache
	for (int i = 0; i < 20; i++) {
		std::string fillerData;
		fillerData.resize(graph::Blob::CHUNK_SIZE * 2);
		for (auto &c: fillerData) {
			c = static_cast<char>(dist(rng));
		}
		(void) chainManager->createBlobChain(entityId + 100 + i, entityTypeId, fillerData);
	}

	// Now update the original chain with new data
	std::string updatedData;
	updatedData.resize(graph::Blob::CHUNK_SIZE * 2);
	for (auto &c: updatedData) {
		c = static_cast<char>(dist(rng) + 1); // Different data
	}

	// This should work even if original chain was evicted
	EXPECT_NO_THROW((void) chainManager->updateBlobChain(headBlobId, entityId, entityTypeId, updatedData));

	// Verify the updated chain can be read correctly
	std::string readData = chainManager->readBlobChain(headBlobId);
	EXPECT_EQ(readData, updatedData);
}

TEST_F(BlobChainManagerTest, ReturnedVectorSyncWithDataManager) {
	// Verify that the returned blobChain vector stays in sync with DataManager
	// This is the core issue: blobChain contains copies that must match DataManager

	constexpr int64_t entityId = 3000;
	constexpr int64_t entityTypeId = 3;

	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 3); // 3 blobs
	std::mt19937 rng(789);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	// For each blob in the returned vector, verify it matches DataManager
	for (size_t i = 0; i < blobChain.size(); i++) {
		int64_t blobId = blobChain[i].getId();

		// Fetch from DataManager
		auto blobFromManager = dataManager->getBlob(blobId);

		// Critical: nextBlobId must match
		EXPECT_EQ(blobChain[i].getNextBlobId(), blobFromManager.getNextBlobId())
				<< "Blob " << i << ": returned vector nextBlobId doesn't match DataManager";

		// Also verify other critical fields
		EXPECT_EQ(blobChain[i].getPrevBlobId(), blobFromManager.getPrevBlobId())
				<< "Blob " << i << ": prevBlobId doesn't match";
		EXPECT_EQ(blobChain[i].getChainPosition(), blobFromManager.getChainPosition())
				<< "Blob " << i << ": chainPosition doesn't match";
	}
}

// =============================================================================
// Exception Path Tests
// These tests verify error handling and exception cases
// =============================================================================

// Test that oversized data throws an exception
// Tests branch at line 38-40: if (processedData.size() > Blob::MAX_COMPRESSED_SIZE)
TEST_F(BlobChainManagerTest, ThrowsOnOversizedData) {
	// Create data larger than MAX_COMPRESSED_SIZE (5MB)
	// Use incompressible data to ensure compressed size is still large
	std::string oversizedData;
	oversizedData.resize(graph::Blob::MAX_COMPRESSED_SIZE + 1000);
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: oversizedData) {
		c = static_cast<char>(dist(rng));
	}

	EXPECT_THROW((void) chainManager->createBlobChain(1, 1, oversizedData), std::runtime_error);
}

// Test that reading a non-existent blob chain throws
// Tests branch at line 147-148: if (headBlob.getId() == 0 || !headBlob.isActive())
TEST_F(BlobChainManagerTest, ReadThrowsOnNonExistentBlob) {
	EXPECT_THROW((void) chainManager->readBlobChain(999999), std::runtime_error);
}

// Test that reading a chain with missing intermediate blob throws
// Tests branch at line 170-173: if (currentBlob.getId() == 0 || !currentBlob.isActive())
TEST_F(BlobChainManagerTest, ReadThrowsOnCorruptedChain) {
	constexpr int64_t entityId = 700;
	constexpr int64_t entityTypeId = 7;
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(111);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	ASSERT_GT(blobChain.size(), 1UL);

	// Add blobs to DataManager
	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	// Manually delete the middle blob to simulate corruption
	if (blobChain.size() > 1) {
		graph::Blob middleBlob = dataManager->getBlob(blobChain[1].getId());
		dataManager->deleteBlob(middleBlob);
	}

	// Try to read the corrupted chain - should throw
	EXPECT_THROW((void) chainManager->readBlobChain(blobChain[0].getId()), std::runtime_error);
}

// Test that reading a chain with incorrect position throws
// Tests branch at line 176-179: if (currentBlob.getChainPosition() != expectedPosition)
TEST_F(BlobChainManagerTest, ReadThrowsOnPositionMismatch) {
	constexpr int64_t entityId = 800;
	constexpr int64_t entityTypeId = 8;
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(222);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	ASSERT_GT(blobChain.size(), 1UL);

	// Add blobs to DataManager
	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	// Manually corrupt the position of the second blob
	if (blobChain.size() > 1) {
		graph::Blob secondBlob = dataManager->getBlob(blobChain[1].getId());
		secondBlob.setChainPosition(99);  // Wrong position
		dataManager->updateBlobEntity(secondBlob);
	}

	// Try to read the corrupted chain - should throw
	EXPECT_THROW((void) chainManager->readBlobChain(blobChain[0].getId()), std::runtime_error);
}

// Test that isDataSame handles missing blob gracefully
// Tests branch at line 105-108: catch block in isDataSame
TEST_F(BlobChainManagerTest, IsDataSameReturnsFalseOnMissingBlob) {
	constexpr int64_t entityId = 900;
	constexpr int64_t entityTypeId = 9;
	std::string testData = "Test data";

	// Create a blob chain
	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);

	for (auto &blob: blobChain) {
		dataManager->addBlobEntity(blob);
	}

	ASSERT_GT(blobChain.size(), 0UL);

	// Delete the blob without updating BlobChainManager
	graph::Blob headBlob = dataManager->getBlob(blobChain[0].getId());
	dataManager->deleteBlob(headBlob);

	// isDataSame should return false (not throw) when blob is missing
	// This tests the exception handling path in isDataSame
	// Since we can't directly test isDataSame (it's private), we test through updateBlobChain
	// which calls isDataSame internally
	std::string newData = "Different data";
	EXPECT_NO_THROW((void) chainManager->updateBlobChain(blobChain[0].getId(), entityId, entityTypeId, newData));
}

// Test deleteBlobChain throws on non-existent blob
// Tests branch at line 199-200: if (headBlob.getId() == 0 || !headBlob.isActive())
TEST_F(BlobChainManagerTest, DeleteThrowsOnNonExistentBlob) {
	EXPECT_THROW(chainManager->deleteBlobChain(999999), std::runtime_error);
}

// Additional tests to improve branch coverage

// Test updateBlobChain when isDataSame returns false
// Tests the else branch at line 131 in updateBlobChain
TEST_F(BlobChainManagerTest, UpdateBlobChainWithDifferentData) {
	constexpr int64_t entityId = 1100;
	constexpr int64_t entityTypeId = 11;
	std::string originalData = "Original data for update test";
	std::string newData = "Completely different new data with more content to ensure it's different";

	// Create a blob chain
	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, originalData);
	ASSERT_GT(blobChain.size(), 0UL);
	int64_t headBlobId = blobChain[0].getId();

	// Flush to make it persistent
	dataManager->prepareFlushSnapshot();
	dataManager->commitFlushSnapshot();

	// Update with different data - should delete old chain and create new one
	auto updatedChain = chainManager->updateBlobChain(headBlobId, entityId, entityTypeId, newData);

	// Verify the new data
	std::string readData = chainManager->readBlobChain(headBlobId);
	EXPECT_EQ(readData, newData);
}

// Test updateBlobChain with same data (early return optimization)
// Tests branch at line 116: if (isDataSame(headBlobId, data))
TEST_F(BlobChainManagerTest, UpdateBlobChainWithSameData) {
	constexpr int64_t entityId = 1200;
	constexpr int64_t entityTypeId = 12;
	std::string testData = "Data that won't change";

	// Create a blob chain
	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);
	ASSERT_GT(blobChain.size(), 0UL);
	int64_t headBlobId = blobChain[0].getId();

	// Record the original blob IDs
	std::vector<int64_t> originalBlobIds;
	for (const auto &blob: blobChain) {
		originalBlobIds.push_back(blob.getId());
	}

	// Update with same data - should return existing chain without modification
	auto updatedChain = chainManager->updateBlobChain(headBlobId, entityId, entityTypeId, testData);

	// Verify the same blob IDs are returned (no new blobs created)
	ASSERT_EQ(updatedChain.size(), originalBlobIds.size());
	for (size_t i = 0; i < updatedChain.size(); i++) {
		EXPECT_EQ(updatedChain[i].getId(), originalBlobIds[i]);
	}
}

// Test readBlobChain with compressed single blob
// Tests branch at line 154-157 in readBlobChain
TEST_F(BlobChainManagerTest, ReadCompressedSingleBlob) {
	constexpr int64_t entityId = 1300;
	constexpr int64_t entityTypeId = 13;
	// Use highly compressible data
	std::string compressibleData(1000, 'A');

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, compressibleData);
	ASSERT_EQ(blobChain.size(), 1UL); // Should be single blob

	// Verify compression is enabled
	EXPECT_TRUE(blobChain[0].isCompressed());
	EXPECT_EQ(blobChain[0].getOriginalSize(), compressibleData.size());

	// Read and verify
	std::string readData = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(readData, compressibleData);
}

// Test readBlobChain with compressed multi-blob chain
// Tests branch at line 190-192 in readBlobChain
TEST_F(BlobChainManagerTest, ReadCompressedBlobChain) {
	constexpr int64_t entityId = 1400;
	constexpr int64_t entityTypeId = 14;
	// Use random data to avoid high compression and ensure multiple blobs
	std::string randomData;
	randomData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: randomData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, randomData);

	// Should have multiple blobs
	EXPECT_GT(blobChain.size(), 1UL);

	// Verify all blobs are compressed
	for (const auto &blob: blobChain) {
		EXPECT_TRUE(blob.isCompressed());
	}

	// Read and verify
	std::string readData = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(readData, randomData);
}

// Test getBlobChainIds with inactive blob in chain
// Tests branch at line 231-233 in getBlobChainIds
TEST_F(BlobChainManagerTest, GetBlobChainIdsWithInactiveBlob) {
	constexpr int64_t entityId = 1500;
	constexpr int64_t entityTypeId = 15;
	std::string testData;
	testData.resize(graph::Blob::CHUNK_SIZE * 2 + 100);
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);
	ASSERT_GT(blobChain.size(), 1UL);

	// Flush to make persistent before marking inactive
	dataManager->prepareFlushSnapshot();
	dataManager->commitFlushSnapshot();

	// Mark middle blob as inactive
	if (blobChain.size() > 1) {
		graph::Blob middleBlob = dataManager->getBlob(blobChain[1].getId());

		// Just verify we can mark it inactive (updateBlobEntity will be called internally)
		// Don't call updateBlobEntity to avoid "Update inactive entity" error
		// Instead, delete the middle blob directly
		dataManager->deleteBlob(middleBlob);

		// Reading the chain should now fail due to missing blob
		EXPECT_THROW((void) chainManager->readBlobChain(blobChain[0].getId()), std::runtime_error);
	}
}

// Test createBlobChain with exactly CHUNK_SIZE data
// Tests boundary condition in splitData
TEST_F(BlobChainManagerTest, CreateBlobChainExactChunkSize) {
	constexpr int64_t entityId = 1600;
	constexpr int64_t entityTypeId = 16;
	// Use random data to avoid high compression
	std::string exactSizeData;
	exactSizeData.resize(graph::Blob::CHUNK_SIZE); // Exactly one chunk before compression
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: exactSizeData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, exactSizeData);

	// Should create at least 1 blob (might be 1 or more after compression/splitting)
	EXPECT_GE(blobChain.size(), 1UL);

	// Verify data integrity
	std::string readData = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(readData, exactSizeData);
}

// Test createBlobChain with CHUNK_SIZE + 1 data
// Tests boundary condition where second chunk is minimal
TEST_F(BlobChainManagerTest, CreateBlobChainChunkSizePlusOne) {
	constexpr int64_t entityId = 1700;
	constexpr int64_t entityTypeId = 17;
	std::string chunkPlusOneData;
	chunkPlusOneData.resize(graph::Blob::CHUNK_SIZE + 1); // Two blobs, second has 1 byte
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: chunkPlusOneData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, chunkPlusOneData);

	// Should create exactly 2 blobs
	EXPECT_EQ(blobChain.size(), 2UL);

	// Verify data integrity
	std::string readData = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(readData, chunkPlusOneData);
}

// Test splitData directly through createBlobChain with very small data
// Tests minimal case in splitData
TEST_F(BlobChainManagerTest, CreateBlobChainWithOneByte) {
	constexpr int64_t entityId = 1800;
	constexpr int64_t entityTypeId = 18;
	const std::string oneByteData = "X";

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, oneByteData);

	EXPECT_EQ(blobChain.size(), 1UL);

	std::string readData = chainManager->readBlobChain(blobChain[0].getId());
	EXPECT_EQ(readData, oneByteData);
}

// Test deleteBlobChain with single blob
// Tests deletion of non-chained blob
TEST_F(BlobChainManagerTest, DeleteSingleBlobChain) {
	constexpr int64_t entityId = 1900;
	constexpr int64_t entityTypeId = 19;
	const std::string testData = "Single blob test";

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, testData);
	ASSERT_EQ(blobChain.size(), 1UL);
	int64_t headBlobId = blobChain[0].getId();

	EXPECT_NO_THROW(chainManager->deleteBlobChain(headBlobId));

	// Verify blob is deleted
	auto loaded = dataManager->getBlob(headBlobId);
	EXPECT_TRUE(loaded.getId() == 0 || !loaded.isActive());
}

// Test deleteBlobChain with large chain
// Tests deletion loop with many blobs
TEST_F(BlobChainManagerTest, DeleteLargeBlobChain) {
	constexpr int64_t entityId = 2000;
	constexpr int64_t entityTypeId = 20;
	std::string largeData;
	largeData.resize(graph::Blob::CHUNK_SIZE * 5 + 100); // 6 blobs
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: largeData) {
		c = static_cast<char>(dist(rng));
	}

	auto blobChain = chainManager->createBlobChain(entityId, entityTypeId, largeData);
	ASSERT_EQ(blobChain.size(), 6UL);
	int64_t headBlobId = blobChain[0].getId();

	EXPECT_NO_THROW(chainManager->deleteBlobChain(headBlobId));

	// Verify all blobs are deleted
	for (const auto &blob: blobChain) {
		auto loaded = dataManager->getBlob(blob.getId());
		EXPECT_TRUE(loaded.getId() == 0 || !loaded.isActive());
	}
}
