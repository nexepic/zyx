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
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::vector {
	namespace {
		constexpr size_t VECTOR_BLOB_PTRS_ENCODED_SIZE = sizeof(int64_t) * 3;

		int64_t requireBlobHeadId(const std::vector<Blob> &blobs, const char *operation) {
			if (blobs.empty()) { // ZYX_COV_EXCL_LINE: BlobChainManager always creates a head blob; fail fast if the storage contract changes.
				throw std::runtime_error(std::string(operation) + " did not create a blob head");
			}
			return blobs.front().getId();
		}

		void writeInt64LittleEndian(std::array<char, VECTOR_BLOB_PTRS_ENCODED_SIZE> &bytes, size_t &offset,
									int64_t value) {
			const uint64_t unsignedValue = static_cast<uint64_t>(value);
			for (size_t index = 0; index < sizeof(uint64_t); ++index)
				bytes[offset++] = static_cast<char>((unsignedValue >> (index * 8U)) & 0xFFU);
		}

		int64_t readInt64LittleEndian(const std::string &bytes, size_t &offset) {
			uint64_t value = 0;
			for (size_t index = 0; index < sizeof(uint64_t); ++index)
				value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset++])) << (index * 8U);
			if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
				throw std::runtime_error("Vector blob pointer is outside the supported ID range");
			return static_cast<int64_t>(value);
		}

		std::string serializeBlobPtrs(const VectorBlobPtrs &ptrs) {
			if (ptrs.rawBlob < 0 || ptrs.pqBlob < 0 || ptrs.adjBlob < 0)
				throw std::invalid_argument("Vector blob pointers must be non-negative");

			std::array<char, VECTOR_BLOB_PTRS_ENCODED_SIZE> bytes{};
			size_t offset = 0;
			writeInt64LittleEndian(bytes, offset, ptrs.rawBlob);
			writeInt64LittleEndian(bytes, offset, ptrs.pqBlob);
			writeInt64LittleEndian(bytes, offset, ptrs.adjBlob);
			return {bytes.data(), bytes.size()};
		}

		VectorBlobPtrs deserializeBlobPtrs(const std::string &data) {
			if (data.size() != VECTOR_BLOB_PTRS_ENCODED_SIZE)
				throw std::runtime_error("Invalid vector blob-pointer payload size");

			size_t offset = 0;
			VectorBlobPtrs ptrs;
			ptrs.rawBlob = readInt64LittleEndian(data, offset);
			ptrs.pqBlob = readInt64LittleEndian(data, offset);
			ptrs.adjBlob = readInt64LittleEndian(data, offset);
			return ptrs;
		}
	} // namespace

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
		if (ids.size() != 1)
			throw std::runtime_error("Vector mapping contains multiple metadata blobs for one node");

		// Read MetaBlob (Indirection)
		std::string data = dataManager_->getBlobManager()->readBlobChain(ids[0]);
		VectorBlobPtrs ptrs = deserializeBlobPtrs(data);

		{
			std::unique_lock lock(cacheMutex_);
			mappingCache_.put(nodeId, ptrs);
		}
		return ptrs;
	}

	void VectorIndexRegistry::setBlobPtrs(int64_t nodeId, const VectorBlobPtrs &ptrs) {
		std::string data = serializeBlobPtrs(ptrs);
		std::vector<int64_t> ids = mappingTree_->find(config_.mappingIndexId, PropertyValue(nodeId));

		if (!ids.empty()) {
			if (ids.size() != 1)
				throw std::runtime_error("Vector mapping contains multiple metadata blobs for one node");

			auto replacement = dataManager_->getBlobManager()->createBlobChain(nodeId, 0, data);
			const int64_t replacementHeadId = requireBlobHeadId(replacement, "vector mapping replacement");
			const int64_t oldHeadId = ids.front();
			try {
				if (!mappingTree_->replaceUniqueValue(config_.mappingIndexId, PropertyValue(nodeId), oldHeadId,
														 replacementHeadId)) {
					throw std::runtime_error("Vector mapping changed before its metadata replacement could be published");
				}
			} catch (...) {
				dataManager_->getBlobManager()->deleteBlobChain(replacementHeadId);
				throw;
			}
			dataManager_->getBlobManager()->deleteBlobChain(oldHeadId);
		} else {
			auto blobs = dataManager_->getBlobManager()->createBlobChain(nodeId, 0, data);
			const int64_t blobHeadId = requireBlobHeadId(blobs, "vector mapping");
			// Capture and persist root changes when the mapping B+Tree splits.
			int64_t newRoot = mappingTree_->insert(config_.mappingIndexId, PropertyValue(nodeId), blobHeadId);
			if (newRoot != config_.mappingIndexId) {
				config_.mappingIndexId = newRoot;
				saveConfig();
			}
		}

		{
			std::unique_lock lock(cacheMutex_);
			mappingCache_.put(nodeId, ptrs);
		}
	}

	bool VectorIndexRegistry::removeBlobPtrs(const int64_t nodeId) {
		const auto ids = mappingTree_->find(config_.mappingIndexId, PropertyValue(nodeId));
		if (ids.empty()) {
			std::unique_lock lock(cacheMutex_);
			mappingCache_.remove(nodeId);
			return false;
		}
		if (ids.size() != 1)
			throw std::runtime_error("Vector mapping contains multiple metadata blobs for one node");

		const int64_t metadataBlobId = ids.front();
		int64_t rootId = config_.mappingIndexId;
		if (!mappingTree_->remove(rootId, PropertyValue(nodeId), metadataBlobId))
			throw std::runtime_error("Vector mapping disappeared before it could be removed");
		if (rootId != config_.mappingIndexId) {
			config_.mappingIndexId = rootId;
			saveConfig();
		}
		dataManager_->getBlobManager()->deleteBlobChain(metadataBlobId);

		std::unique_lock lock(cacheMutex_);
		mappingCache_.remove(nodeId);
		return true;
	}

	void VectorIndexRegistry::deleteBlobChain(const int64_t blobId) const {
		if (blobId != 0)
			dataManager_->getBlobManager()->deleteBlobChain(blobId);
	}

	int64_t VectorIndexRegistry::saveRawVector(const std::vector<BFloat16> &vec) const {
		std::string data(vec.size() * sizeof(BFloat16), '\0');
		if (!vec.empty())
			std::memcpy(data.data(), vec.data(), data.size());
		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return requireBlobHeadId(blobs, "raw vector");
	}

	std::vector<BFloat16> VectorIndexRegistry::loadRawVector(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		if (data.size() % sizeof(BFloat16) != 0)
			throw std::runtime_error("Raw vector blob has an invalid byte length");
		size_t n = data.size() / sizeof(BFloat16);
		std::vector<BFloat16> vec(n);
		std::memcpy(vec.data(), data.data(), data.size());
		return vec;
	}

	int64_t VectorIndexRegistry::savePQCodes(const std::vector<uint8_t> &codes) const {
		std::string data(codes.size(), '\0');
		if (!codes.empty())
			std::memcpy(data.data(), codes.data(), data.size());
		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return requireBlobHeadId(blobs, "PQ codes");
	}

	std::vector<uint8_t> VectorIndexRegistry::loadPQCodes(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		return {data.begin(), data.end()};
	}

	int64_t VectorIndexRegistry::saveAdjacency(const std::vector<int64_t> &neighbors) const {
		if (neighbors.size() > std::numeric_limits<uint32_t>::max() ||
			neighbors.size() > (std::numeric_limits<size_t>::max() - sizeof(uint32_t)) / sizeof(int64_t)) {
			throw std::length_error("Adjacency list exceeds the supported serialized size");
		}
		const size_t dataSize = sizeof(uint32_t) + neighbors.size() * sizeof(int64_t);
		std::string data(dataSize, '\0');
		char *ptr = data.data();

		auto count = static_cast<uint32_t>(neighbors.size());
		std::memcpy(ptr, &count, sizeof(uint32_t));
		ptr += sizeof(uint32_t);

		if (!neighbors.empty())
			std::memcpy(ptr, neighbors.data(), neighbors.size() * sizeof(int64_t));

		auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, data);
		return requireBlobHeadId(blobs, "adjacency list");
	}

	std::vector<int64_t> VectorIndexRegistry::loadAdjacency(int64_t blobId) const {
		if (blobId == 0)
			return {};
		std::string data = dataManager_->getBlobManager()->readBlobChain(blobId);
		if (data.size() < sizeof(uint32_t))
			throw std::runtime_error("Adjacency blob is missing its neighbor count");

		const char *ptr = data.data();
		uint32_t count;
		std::memcpy(&count, ptr, sizeof(uint32_t));
		ptr += sizeof(uint32_t);

		const size_t availableBytes = data.size() - sizeof(uint32_t);
		if (count > availableBytes / sizeof(int64_t) ||
			static_cast<size_t>(count) * sizeof(int64_t) != availableBytes) {
			throw std::runtime_error("Adjacency blob has an invalid neighbor payload length");
		}

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
			// Vector mapping indexes are created with INTEGER keys; enforce that invariant at read time.
			nodeIds.push_back(std::get<int64_t>(k.getVariant()));
		}
		return nodeIds;
	}
} // namespace graph::vector
