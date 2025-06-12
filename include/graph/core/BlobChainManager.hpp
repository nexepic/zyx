/**
 * @file BlobChainManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <vector>
#include <string>
#include <memory>

namespace graph {
	class Blob;
	namespace storage {
		class DataManager;
	}

	class BlobChainManager {
	public:
		explicit BlobChainManager(std::shared_ptr<storage::DataManager> dataManager);

		// Split data into multiple blob entities
		std::vector<Blob> createBlobChain(int64_t entityId, uint32_t entityType, const std::string &data) const;

		// Reassemble data from a chain of blob entities
		std::string readBlobChain(int64_t headBlobId) const;

		// Delete an entire chain of blob entities
		void deleteBlobChain(int64_t headBlobId) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;

		// Compress data and check size limits
		static std::string compressData(const std::string &data);

		// Split data into chunks that fit within CHUNK_SIZE
		static std::vector<std::string> splitData(const std::string &data);

		// Get all blob IDs in a chain
		std::vector<int64_t> getBlobChainIds(int64_t headBlobId) const;
	};
}