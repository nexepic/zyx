/**
 * @file BlobChainManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		// Determine if this is an Index blob
		bool isIndexBlob = (entityType == storage::Index::typeId);

		// Compress the data first (unless it's an index blob)
		std::string processedData;
		// TODO: Decouple Index blob compression logic from BlobChainManager
		if (isIndexBlob) {
			// For index blobs, skip compression and size limits
			processedData = data;
		} else {
			// For other entities, compress and check size
			processedData = compressData(data);

			// Check size limit for non-index blobs
			if (processedData.size() > Blob::MAX_COMPRESSED_SIZE) {
				throw std::runtime_error("Compressed data exceeds maximum size limit of 5MB");
			}
		}

		// Split into chunks
		auto chunks = splitData(processedData);

		// Create blob entities
		std::vector<Blob> blobChain;
		blobChain.reserve(chunks.size());

		int64_t prevBlobId = 0;

		// Create each blob in the chain
		for (size_t i = 0; i < chunks.size(); i++) {
			// Create new blob with temporary ID
			int64_t tempId = dataManager_->reserveTemporaryBlobId();
			Blob blob(tempId, chunks[i]);

			// Set entity info
			blob.setEntityInfo(entityId, entityType);

			// TODO: Decouple Index blob compression logic from BlobChainManager
			// Set compression info - only set compressed flag for non-index blobs
			blob.setCompressionInfo(data.size(), !isIndexBlob);

			// Set chain position
			blob.setChainPosition(static_cast<int32_t>(i));

			// Set previous blob ID
			blob.setPrevBlobId(prevBlobId);

			// Add to chain
			blobChain.push_back(blob);

			// Update previous blob's next pointer if not the first blob
			if (i > 0) {
				blobChain[i - 1].setNextBlobId(tempId);
			}

			prevBlobId = tempId;
		}

		return blobChain;
	}

	std::string BlobChainManager::readBlobChain(int64_t headBlobId) const {
		// Get the head blob
		Blob headBlob = dataManager_->getBlob(headBlobId);
		if (headBlob.getId() == 0 || !headBlob.isActive()) {
			throw std::runtime_error("Head blob not found or inactive");
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
