/**
 * @file BlobChainManager.hpp
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

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace graph {
	class Blob;
	namespace storage {
		class DataManager;
	}

	class BlobChainManager {
	public:
		explicit BlobChainManager(std::shared_ptr<storage::DataManager> dataManager);

		// Split data into multiple blob entities
		[[nodiscard]] std::vector<Blob> createBlobChain(int64_t entityId, uint32_t entityType,
														const std::string &data) const;

		[[nodiscard]] bool isDataSame(int64_t headBlobId, const std::string &newData) const;

		[[nodiscard]] std::vector<Blob> updateBlobChain(int64_t headBlobId, int64_t entityId, uint32_t entityType,
														const std::string &data) const;

		// Reassemble data from a chain of blob entities
		[[nodiscard]] std::string readBlobChain(int64_t headBlobId) const;

		// Delete an entire chain of blob entities
		void deleteBlobChain(int64_t headBlobId) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;

		// Compress data and check size limits
		static std::string compressData(const std::string &data);

		// Split data into chunks that fit within CHUNK_SIZE
		static std::vector<std::string> splitData(const std::string &data);

		// Get all blob IDs in a chain
		[[nodiscard]] std::vector<int64_t> getBlobChainIds(int64_t headBlobId) const;
	};
} // namespace graph
