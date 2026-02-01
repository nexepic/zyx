/**
 * @file BlobChainManager.cpp
 * @author Nexepic
 * @date 2025/5/20
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

#include "graph/core/BlobChainManager.hpp"
#include <stdexcept>
#include "graph/core/Blob.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/utils/CompressUtils.hpp"

namespace graph {

	BlobChainManager::BlobChainManager(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::vector<Blob> BlobChainManager::createBlobChain(int64_t entityId, uint32_t entityType,
														const std::string &data) const {
		// Compress all data regardless of entity type
		std::string processedData = compressData(data);

		// Check size limit for all blobs
		if (processedData.size() > Blob::MAX_COMPRESSED_SIZE) {
			throw std::runtime_error("Compressed data exceeds maximum size limit of 5MB");
		}

		// Split into chunks
		auto chunks = splitData(processedData);
		if (chunks.empty()) {
			return {};
		}

		// Create blob entities
		std::vector<Blob> blobChain;
		blobChain.reserve(chunks.size());

		// First pass: Create all blobs and collect their IDs
		// This separates blob creation from linking to avoid cache eviction issues
		std::vector<int64_t> blobIds;
		blobIds.reserve(chunks.size());

		for (size_t i = 0; i < chunks.size(); i++) {
			Blob currentBlob(0, chunks[i]);
			currentBlob.setEntityInfo(entityId, entityType);
			currentBlob.setCompressionInfo(data.size(), true);
			currentBlob.setChainPosition(static_cast<int32_t>(i));
			currentBlob.setPrevBlobId(i > 0 ? blobIds[i - 1] : 0);

			dataManager_->addBlobEntity(currentBlob);
			blobIds.push_back(currentBlob.getId());
			blobChain.push_back(currentBlob);
		}

		// Second pass: Update nextBlobId for all blobs except the last
		// By doing this after all blobs are created, we avoid issues with cache eviction
		for (size_t i = 0; i < blobIds.size() - 1; i++) {
			int64_t currentId = blobIds[i];
			Blob blob = dataManager_->getBlob(currentId);
			if (blob.getId() != 0 && blob.isActive()) {
				blob.setNextBlobId(blobIds[i + 1]);
				dataManager_->updateBlobEntity(blob);
				// Also update the blob in the returned vector to maintain consistency
				blobChain[i].setNextBlobId(blobIds[i + 1]);
			}
		}

		return blobChain;
	}

	bool BlobChainManager::isDataSame(int64_t headBlobId, const std::string &newData) const {
		try {
			const std::string currentData = readBlobChain(headBlobId);
			return currentData == newData;
		} catch (const std::exception &) {
			// If we can't read the current data, assume it's different
			return false;
		}
	}

	std::vector<Blob> BlobChainManager::updateBlobChain(int64_t headBlobId, int64_t entityId, uint32_t entityType,
														const std::string &data) const {
		// First check if the data is actually different
		// If we can't read the current data (e.g., blob evicted from cache and not yet flushed),
		// isDataSame will return false, and we'll proceed to create a new chain
		if (isDataSame(headBlobId, data)) {
			// Data is the same, return the existing chain
			const auto chainIds = getBlobChainIds(headBlobId);
			std::vector<Blob> existingChain;
			existingChain.reserve(chainIds.size());

			for (auto blobId: chainIds) {
				Blob blob = dataManager_->getBlob(blobId);
				if (blob.getId() != 0 && blob.isActive()) {
					existingChain.push_back(blob);
				}
			}
			return existingChain;
		}

		// Data is different or old blob is not accessible, proceed with update
		Blob headBlob = dataManager_->getBlob(headBlobId);
		if (headBlob.getId() != 0 && headBlob.isActive()) {
			// Old blob exists and is active, delete the existing chain first
			deleteBlobChain(headBlobId);
		}
		// If old blob doesn't exist or is inactive, just create a new chain
		// (this can happen if the blob was evicted from cache and not yet flushed to disk)

		// Create a new chain with updated data
		return createBlobChain(entityId, entityType, data);
	}

	std::string BlobChainManager::readBlobChain(int64_t headBlobId) const {
		// Get the head blob
		Blob headBlob = dataManager_->getBlob(headBlobId);
		if (headBlob.getId() == 0 || !headBlob.isActive()) {
			throw std::runtime_error("Head blob not found: " + std::to_string(headBlobId));
		}

		// Check if this is a single blob or a chain
		if (!headBlob.isChained()) {
			// Single blob case
			if (headBlob.isCompressed()) {
				// Decompress and return
				return utils::zlibDecompress(std::string(headBlob.getData(), headBlob.getSize()),
											 headBlob.getOriginalSize());
			}
			return {headBlob.getData(), headBlob.getSize()};
		}

		// Reassemble data from the chain
		std::string reassembledData;
		int64_t currentBlobId = headBlobId;
		int32_t expectedPosition = 0;

		while (currentBlobId != 0) {
			Blob currentBlob = dataManager_->getBlob(currentBlobId);

			if (currentBlob.getId() == 0 || !currentBlob.isActive()) {
				throw std::runtime_error("Blob chain corrupted: missing blob at position " +
										 std::to_string(expectedPosition));
			}

			// Verify chain position
			if (currentBlob.getChainPosition() != expectedPosition) {
				throw std::runtime_error("Blob chain corrupted: expected position " + std::to_string(expectedPosition) +
										 " but found " + std::to_string(currentBlob.getChainPosition()));
			}

			// Append data with explicit size
			reassembledData.append(currentBlob.getData(), currentBlob.getSize());

			// Move to next blob
			currentBlobId = currentBlob.getNextBlobId();
			expectedPosition++;
		}

		// If compressed, decompress the reassembled data
		if (headBlob.isCompressed()) {
			return utils::zlibDecompress(reassembledData, headBlob.getOriginalSize());
		}

		return reassembledData;
	}

	void BlobChainManager::deleteBlobChain(int64_t headBlobId) const {
		const Blob headBlob = dataManager_->getBlob(headBlobId);
		if (headBlob.getId() == 0 || !headBlob.isActive()) {
			throw std::runtime_error("Head blob not found or is inactive for deletion: " + std::to_string(headBlobId));
		}

		// Delete each blob in the chain
		for (const auto chainIds = getBlobChainIds(headBlobId); const auto blobId: chainIds) {
			if (Blob blob = dataManager_->getBlob(blobId); blob.getId() != 0 && blob.isActive()) {
				dataManager_->deleteBlob(blob);
			}
		}
	}

	std::string BlobChainManager::compressData(const std::string &data) { return utils::zlibCompress(data); }

	std::vector<std::string> BlobChainManager::splitData(const std::string &data) {
		std::vector<std::string> chunks;

		for (size_t offset = 0; offset < data.size(); offset += Blob::CHUNK_SIZE) {
			size_t chunkSize = std::min(Blob::CHUNK_SIZE, data.size() - offset);
			chunks.push_back(data.substr(offset, chunkSize));
		}

		return chunks;
	}

	std::vector<int64_t> BlobChainManager::getBlobChainIds(int64_t headBlobId) const {
		std::vector<int64_t> chainIds;
		int64_t currentBlobId = headBlobId;

		while (currentBlobId != 0) {
			Blob currentBlob = dataManager_->getBlob(currentBlobId);

			if (currentBlob.getId() == 0 || !currentBlob.isActive()) {
				break;
			}

			chainIds.push_back(currentBlobId);
			currentBlobId = currentBlob.getNextBlobId();
		}

		return chainIds;
	}

} // namespace graph
