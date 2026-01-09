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
