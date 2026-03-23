/**
 * @file VectorIndexRegistry.cpp
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

#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::vector {

	VectorIndexRegistry::VectorIndexRegistry(std::shared_ptr<storage::DataManager> dm,
											 std::shared_ptr<storage::state::SystemStateManager> sm,
											 const std::string &indexName) :
		dataManager_(std::move(dm)), stateManager_(std::move(sm)), stateKey_(STATE_KEY_PREFIX + indexName),
		mappingCache_(CACHE_SIZE) {

		codebookStateKey_ = stateKey_ + ".codebook"; // Define separate key for codebook
		loadConfig();

		// 1001 is internal ID for Vector Mapping B+Tree
		mappingTree_ = std::make_shared<query::indexes::IndexTreeManager>(
				dataManager_, query::indexes::IndexTypes::VECTOR_MAPPING_INDEX, PropertyType::INTEGER);

		if (config_.mappingIndexId == 0) {
			config_.mappingIndexId = mappingTree_->initialize();
			config_.codebookKey = codebookStateKey_; // Link config to codebook key
			saveConfig();
		}
	}

	void VectorIndexRegistry::loadConfig() {
		auto props = stateManager_->getAll(stateKey_);
		if (props.empty())
			config_ = VectorIndexConfig();
		else
			config_ = VectorIndexConfig::fromProperties(props);
	}

	void VectorIndexRegistry::saveConfig() const {
		// Persist metadata (small data, useBlobStorage=false)
		stateManager_->setMap(stateKey_, config_.toProperties(), storage::state::UpdateMode::MERGE, false);
	}

	void VectorIndexRegistry::updateConfig(const VectorIndexConfig &newConfig) {
		config_ = newConfig;
		saveConfig();
	}

	void VectorIndexRegistry::updateEntryPoint(int64_t nodeId) {
		config_.entryPointNodeId = nodeId;
		saveConfig();
	}

	// --- Quantizer Persistence (The Optimization) ---

	void VectorIndexRegistry::saveQuantizer(const NativeProductQuantizer &pq) {
		std::stringstream ss;
		pq.serialize(ss);
		std::string binaryData = ss.str();

		// Use the new useBlobStorage=true feature to store this large string
		// We store it as a single property "data" under the codebook state key
		stateManager_->set<std::string>(codebookStateKey_, "data", binaryData, true);

		config_.isTrained = true;
		config_.codebookKey = codebookStateKey_;
		saveConfig();
	}

	std::unique_ptr<NativeProductQuantizer> VectorIndexRegistry::loadQuantizer() const {
		if (!config_.isTrained)
			return nullptr;

		// Load large data from Blob via StateManager
		auto binaryData = stateManager_->get<std::string>(codebookStateKey_, "data", "");
		if (binaryData.empty())
			return nullptr;

		std::stringstream ss(binaryData);
		return NativeProductQuantizer::deserialize(ss);
	}

	// --- Blob Logic (Same as before) ---

	VectorBlobPtrs VectorIndexRegistry::getBlobPtrs(int64_t nodeId) {
		{
			std::shared_lock lock(cacheMutex_);
			if (mappingCache_.contains(nodeId))
				return mappingCache_.get(nodeId);
		}
		std::vector<int64_t> ids = mappingTree_->find(config_.mappingIndexId, PropertyValue(nodeId));
		if (ids.empty())
			return {0, 0, 0};

		// Read MetaBlob (Indirection)
		std::string data = dataManager_->getBlobManager()->readBlobChain(ids[0]);
		if (data.size() != sizeof(VectorBlobPtrs))
			return {0, 0, 0};

		VectorBlobPtrs ptrs;
		std::memcpy(&ptrs, data.data(), sizeof(VectorBlobPtrs));

		{
			std::unique_lock lock(cacheMutex_);
			mappingCache_.put(nodeId, ptrs);
		}
		return ptrs;
	}

	void VectorIndexRegistry::setBlobPtrs(int64_t nodeId, const VectorBlobPtrs &ptrs) {
		std::string data(reinterpret_cast<const char *>(&ptrs), sizeof(VectorBlobPtrs));
		std::vector<int64_t> ids = mappingTree_->find(config_.mappingIndexId, PropertyValue(nodeId));

		if (!ids.empty()) {
			(void) dataManager_->getBlobManager()->updateBlobChain(ids[0], nodeId, 0, data);
		} else {
			auto blobs = dataManager_->getBlobManager()->createBlobChain(nodeId, 0, data);
			if (!blobs.empty()) {
				// FIX: Capture new root ID
				int64_t newRoot = mappingTree_->insert(config_.mappingIndexId, PropertyValue(nodeId), blobs[0].getId());

				// Check and update if root changed
				if (newRoot != config_.mappingIndexId) {
					config_.mappingIndexId = newRoot;
					saveConfig(); // Persist new root ID
				}
			}
		}

		{
			std::unique_lock lock(cacheMutex_);
			mappingCache_.put(nodeId, ptrs);
		}
	}

	int64_t VectorIndexRegistry::saveRawVector(const std::vector<BFloat16> &vec) const {
		std::string data(reinterpret_cast<const char *>(vec.data()), vec.size() * sizeof(BFloat16));
		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return blobs.empty() ? 0 : blobs[0].getId();
	}

	std::vector<BFloat16> VectorIndexRegistry::loadRawVector(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		size_t n = data.size() / sizeof(BFloat16);
		std::vector<BFloat16> vec(n);
		std::memcpy(vec.data(), data.data(), data.size());
		return vec;
	}

	int64_t VectorIndexRegistry::savePQCodes(const std::vector<uint8_t> &codes) const {
		std::string data(reinterpret_cast<const char *>(codes.data()), codes.size());
		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return blobs.empty() ? 0 : blobs[0].getId();
	}

	std::vector<uint8_t> VectorIndexRegistry::loadPQCodes(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		return {data.begin(), data.end()};
	}

	int64_t VectorIndexRegistry::saveAdjacency(const std::vector<int64_t> &neighbors) const {
		const size_t dataSize = sizeof(uint32_t) + neighbors.size() * sizeof(int64_t);
		std::string data(dataSize, '\0');
		char *ptr = data.data();

		auto count = static_cast<uint32_t>(neighbors.size());
		std::memcpy(ptr, &count, sizeof(uint32_t));
		ptr += sizeof(uint32_t);

		if (!neighbors.empty())
			std::memcpy(ptr, neighbors.data(), neighbors.size() * sizeof(int64_t));

		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return blobs.empty() ? 0 : blobs[0].getId();
	}

	std::vector<int64_t> VectorIndexRegistry::loadAdjacency(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		if (data.size() < sizeof(uint32_t))
			return {};

		const char *ptr = data.data();
		uint32_t count;
		std::memcpy(&count, ptr, sizeof(uint32_t));
		ptr += sizeof(uint32_t);

		std::vector<int64_t> res(count);
		if (count > 0)
			std::memcpy(res.data(), ptr, count * sizeof(int64_t));
		return res;
	}

	std::vector<int64_t> VectorIndexRegistry::getAllNodeIds(size_t limit) const {
		if (config_.mappingIndexId == 0)
			return {}; // Safety check
		auto keys = mappingTree_->scanKeys(config_.mappingIndexId, limit);
		std::vector<int64_t> nodeIds;
		nodeIds.reserve(keys.size());
		for (const auto &k: keys) {
			if (k.getType() == PropertyType::INTEGER) {
				nodeIds.push_back(std::get<int64_t>(k.getVariant()));
			}
		}
		return nodeIds;
	}
} // namespace graph::vector
