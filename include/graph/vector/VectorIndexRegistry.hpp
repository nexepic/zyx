/**
 * @file VectorIndexRegistry.hpp
 * @author Nexepic
 * @date 2026/1/22
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

#pragma once

#include <memory>
#include <mutex>
#include <sstream>
#include "VectorIndexConfig.hpp"
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/CacheManager.hpp"
#include "graph/vector/core/BFloat16.hpp"
#include "graph/vector/quantization/NativeProductQuantizer.hpp"

namespace graph::storage {
	class DataManager;
	namespace state {
		class SystemStateManager;
	}
} // namespace graph::storage

namespace graph::vector {

	struct VectorBlobPtrs {
		int64_t rawBlob = 0;
		int64_t pqBlob = 0;
		int64_t adjBlob = 0;
	};

	class VectorIndexRegistry {
	public:
		static constexpr char STATE_KEY_PREFIX[] = "sys.vector.idx.";
		static constexpr size_t CACHE_SIZE = 10000;

		VectorIndexRegistry(std::shared_ptr<storage::DataManager> dm,
							std::shared_ptr<storage::state::SystemStateManager> sm, const std::string &indexName);

		// Core Access
		VectorBlobPtrs getBlobPtrs(int64_t nodeId);
		void setBlobPtrs(int64_t nodeId, const VectorBlobPtrs &ptrs);

		// Config & PQ State Management
		const VectorIndexConfig &getConfig() const { return config_; }

		void updateConfig(const VectorIndexConfig &newConfig);

		// Load PQ from Blob storage
		std::unique_ptr<NativeProductQuantizer> loadQuantizer() const;
		// Save PQ to Blob storage (using useBlobStorage=true)
		void saveQuantizer(const NativeProductQuantizer &pq);

		void updateEntryPoint(int64_t nodeId);

		// Blob Helpers
		int64_t saveRawVector(const std::vector<BFloat16> &vec) const;
		int64_t savePQCodes(const std::vector<uint8_t> &codes) const;
		int64_t saveAdjacency(const std::vector<int64_t> &neighbors) const;

		std::vector<BFloat16> loadRawVector(int64_t blobId) const;
		std::vector<uint8_t> loadPQCodes(int64_t blobId) const;
		std::vector<int64_t> loadAdjacency(int64_t blobId) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<storage::state::SystemStateManager> stateManager_;
		std::string stateKey_;
		std::string codebookStateKey_; // Derived key for Codebook Blob

		VectorIndexConfig config_;
		std::shared_ptr<query::indexes::IndexTreeManager> mappingTree_;

		mutable std::mutex cacheMutex_;
		storage::LRUCache<int64_t, VectorBlobPtrs> mappingCache_;

		void loadConfig();
		void saveConfig() const;
	};
} // namespace graph::vector
