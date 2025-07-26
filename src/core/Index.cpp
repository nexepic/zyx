/**
 * @file Index.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Index.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <graph/core/BlobChainManager.hpp>
#include <graph/storage/data/DataManager.hpp>
#include <sstream>
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {

	Index::Index(const int64_t id, NodeType type, const uint32_t indexType) {
		metadata.id = id;
		metadata.nodeType = type;
		metadata.indexType = indexType;
		metadata.keyCount = 0;
		metadata.childCount = 0;
		metadata.level = (type == NodeType::LEAF) ? 0 : 1;
	}

	void Index::setDataStorageType(DataStorageType type) { metadata.dataStorageType = type; }

	Index::DataStorageType Index::getDataStorageType() const { return metadata.dataStorageType; }

	void Index::setBlobId(int64_t blobId) { metadata.blobId = blobId; }

	int64_t Index::getBlobId() const { return metadata.blobId; }

	bool Index::hasBlobStorage() const {
		return getDataStorageType() == DataStorageType::BLOB_CHAIN && getBlobId() != 0;
	}

	void Index::storeDataInBlob(const std::shared_ptr<DataManager> &dataManager, const std::string &data) {
		if (!dataManager) {
			throw std::runtime_error("DataManager is required for blob storage operations");
		}

		// If there's existing blob storage, delete it
		if (hasBlobStorage()) {
			auto blobManager = std::make_unique<BlobChainManager>(dataManager);
			blobManager->deleteBlobChain(getBlobId());
		}

		// Create a new blob chain
		auto blobManager = std::make_unique<BlobChainManager>(dataManager);
		auto blobChain = blobManager->createBlobChain(getId(), // Entity ID
													  typeId, // Entity type (Index)
													  data // Data to store
		);

		if (blobChain.empty()) {
			throw std::runtime_error("Failed to create blob chain for index data");
		}

		// Add all blobs to storage
		for (auto &blob: blobChain) {
			dataManager->addBlobEntity(blob);
		}

		// Update index with blob reference
		setBlobId(blobChain[0].getId());
		setDataStorageType(DataStorageType::BLOB_CHAIN);

		// Clear inline buffer since data is now in blob
		std::memset(dataBuffer, 0, DATA_SIZE);
	}

	std::string Index::retrieveDataFromBlob(const std::shared_ptr<DataManager> &dataManager) const {
		if (!hasBlobStorage()) {
			return {dataBuffer, DATA_SIZE};
		}

		if (!dataManager) {
			throw std::runtime_error("DataManager is required to retrieve blob data");
		}

		auto blobManager = std::make_unique<BlobChainManager>(dataManager);
		return blobManager->readBlobChain(getBlobId());
	}

	// Update the serialize method to include the new fields
	void Index::serialize(std::ostream &os) const {
		// Serialize metadata
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.parentId);
		utils::Serializer::writePOD(os, metadata.nextLeafId);
		utils::Serializer::writePOD(os, metadata.prevLeafId);
		utils::Serializer::writePOD(os, metadata.blobId);
		utils::Serializer::writePOD(os, metadata.indexType);
		utils::Serializer::writePOD(os, metadata.keyCount);
		utils::Serializer::writePOD(os, metadata.childCount);
		utils::Serializer::writePOD(os, metadata.level);
		utils::Serializer::writePOD(os, static_cast<uint8_t>(metadata.nodeType));
		utils::Serializer::writePOD(os, static_cast<uint8_t>(metadata.dataStorageType));
		utils::Serializer::writePOD(os, metadata.isActive);

		// Write data buffer
		os.write(dataBuffer, DATA_SIZE);
	}

	// Update the deserialize method to include the new fields
	Index Index::deserialize(std::istream &is) {
		Index entity;

		// Deserialize metadata
		entity.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.parentId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.nextLeafId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.prevLeafId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.blobId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.indexType = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.keyCount = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.childCount = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.level = utils::Serializer::readPOD<uint8_t>(is);
		entity.metadata.nodeType = static_cast<NodeType>(utils::Serializer::readPOD<uint8_t>(is));
		entity.metadata.dataStorageType = static_cast<DataStorageType>(utils::Serializer::readPOD<uint8_t>(is));
		entity.metadata.isActive = utils::Serializer::readPOD<bool>(is);

		// Read data buffer
		is.read(entity.dataBuffer, DATA_SIZE);

		return entity;
	}

	// Update getAllKeyValues to handle blob storage
	std::vector<Index::KeyValuePair> Index::getAllKeyValues(const std::shared_ptr<DataManager> &dataManager) const {
		if (metadata.nodeType != NodeType::LEAF) {
			return {}; // Only leaf nodes store key-values
		}

		// Handle blob storage if needed
		if (hasBlobStorage()) {
			if (!dataManager) {
				throw std::runtime_error("DataManager is required to retrieve key-values from blob storage");
			}

			std::string blobData = retrieveDataFromBlob(dataManager);
			std::istringstream is(blobData);

			std::vector<KeyValuePair> result;
			result.reserve(metadata.keyCount);

			for (uint32_t i = 0; i < metadata.keyCount; i++) {
				KeyValuePair kvp;
				kvp.key = utils::Serializer::readString(is);

				auto valueCount = utils::Serializer::readPOD<uint32_t>(is);
				kvp.values.reserve(valueCount);

				for (uint32_t j = 0; j < valueCount; j++) {
					kvp.values.push_back(utils::Serializer::readPOD<int64_t>(is));
				}

				result.push_back(kvp);
			}

			return result;
		} else {
			// Use the existing in-memory deserialization
			auto internalKVs = deserializeStringKVs();
			std::vector<KeyValuePair> result;
			result.reserve(internalKVs.size());

			for (const auto &kv: internalKVs) {
				KeyValuePair publicKV;
				publicKV.key = kv.key;
				publicKV.values = kv.values;
				result.push_back(publicKV);
			}

			return result;
		}
	}

	// Update setAllKeyValues to handle blob storage
	void Index::setAllKeyValues(const std::vector<KeyValuePair> &keyValues,
								const std::shared_ptr<DataManager> &dataManager) {
		if (metadata.nodeType != NodeType::LEAF) {
			return; // Only leaf nodes store key-values
		}

		std::vector<KeyValuePair> internalKVs;
		internalKVs.reserve(keyValues.size());

		for (const auto &kv: keyValues) {
			KeyValuePair internalKV;
			internalKV.key = kv.key;
			internalKV.values = kv.values;
			internalKVs.push_back(internalKV);
		}

		// Serialize to a string first to check size
		std::ostringstream os;
		for (const auto &kvp: internalKVs) {
			utils::Serializer::writeString(os, kvp.key);
			utils::Serializer::writePOD(os, static_cast<uint32_t>(kvp.values.size()));

			for (int64_t value: kvp.values) {
				utils::Serializer::writePOD(os, value);
			}
		}
		std::string data = os.str();
		metadata.keyCount = static_cast<uint32_t>(internalKVs.size());

		// If data fits in buffer, store it directly
		if (data.size() <= DATA_SIZE) {
			setDataStorageType(DataStorageType::INLINE);
			std::memset(dataBuffer, 0, DATA_SIZE);
			std::memcpy(dataBuffer, data.data(), data.size());
		} else {
			// Data too large, store in blob if DataManager is available
			if (dataManager) {
				storeDataInBlob(dataManager, data);
			} else {
				throw std::runtime_error(
						"Data size exceeds buffer capacity and DataManager not provided for blob storage");
			}
		}
	}

	std::vector<Index::KeyValuePair> Index::deserializeStringKVs() const {
		std::vector<KeyValuePair> result;
		if (metadata.keyCount == 0)
			return result;

		std::istringstream is(std::string(dataBuffer, DATA_SIZE));

		for (uint32_t i = 0; i < metadata.keyCount; i++) {
			KeyValuePair kvp;
			kvp.key = utils::Serializer::readString(is);

			auto valueCount = utils::Serializer::readPOD<uint32_t>(is);
			kvp.values.reserve(valueCount);

			for (uint32_t j = 0; j < valueCount; j++) {
				kvp.values.push_back(utils::Serializer::readPOD<int64_t>(is));
			}

			result.push_back(kvp);
		}

		return result;
	}

	void Index::serializeStringKVs(const std::vector<KeyValuePair> &kvs) {
		std::ostringstream os;

		for (const auto &kvp: kvs) {
			utils::Serializer::writeString(os, kvp.key);
			utils::Serializer::writePOD(os, static_cast<uint32_t>(kvp.values.size()));

			for (int64_t value: kvp.values) {
				utils::Serializer::writePOD(os, value);
			}
		}

		std::string data = os.str();
		if (data.size() > DATA_SIZE) {
			throw std::runtime_error("Data size exceeds buffer capacity in Index");
		}

		metadata.keyCount = static_cast<uint32_t>(kvs.size());
		std::memset(dataBuffer, 0, DATA_SIZE);
		std::memcpy(dataBuffer, data.data(), data.size());
	}

	std::vector<Index::ChildEntry> Index::deserializeChildren() const {
		std::vector<ChildEntry> result;
		if (metadata.childCount == 0)
			return result;

		std::istringstream is(std::string(dataBuffer, DATA_SIZE));

		for (uint32_t i = 0; i < metadata.childCount; i++) {
			ChildEntry entry;
			entry.key = utils::Serializer::readString(is);
			entry.childId = utils::Serializer::readPOD<int64_t>(is);
			result.push_back(entry);
		}

		return result;
	}

	void Index::serializeChildren(const std::vector<ChildEntry> &children) {
		std::ostringstream os;

		for (const auto &entry: children) {
			utils::Serializer::writeString(os, entry.key);
			utils::Serializer::writePOD(os, entry.childId);
		}

		std::string data = os.str();
		if (data.size() > DATA_SIZE) {
			throw std::runtime_error("Child entries exceed buffer capacity in Index");
		}

		metadata.childCount = static_cast<uint32_t>(children.size());
		std::memset(dataBuffer, 0, DATA_SIZE);
		std::memcpy(dataBuffer, data.data(), data.size());
	}

	bool Index::insertStringKey(const std::string &key, int64_t value) {
		if (metadata.nodeType != NodeType::LEAF) {
			return false; // Only leaf nodes can store values
		}

		auto kvs = deserializeStringKVs();

		// Find key or insert position
		auto it = std::ranges::find_if(kvs, [&key](const KeyValuePair &kvp) { return kvp.key == key; });

		if (it != kvs.end()) {
			// Key exists, add value if it doesn't already exist
			if (std::ranges::find(it->values, value) == it->values.end()) {
				it->values.push_back(value);
				serializeStringKVs(kvs);
				return true;
			}
			return false; // Value already exists
		} else {
			// Key doesn't exist, insert new key-value pair
			KeyValuePair newKvp;
			newKvp.key = key;
			newKvp.values.push_back(value);

			// Find sorted position
			auto insertPos = std::ranges::lower_bound(
					kvs, newKvp, [](const KeyValuePair &a, const KeyValuePair &b) { return a.key < b.key; });

			kvs.insert(insertPos, newKvp);
			serializeStringKVs(kvs);
			return true;
		}
	}

	bool Index::removeStringKey(const std::string &key, int64_t value) {
		if (metadata.nodeType != NodeType::LEAF) {
			return false; // Only leaf nodes can store values
		}

		auto kvs = deserializeStringKVs();

		// Find key
		auto it = std::ranges::find_if(kvs, [&key](const KeyValuePair &kvp) { return kvp.key == key; });

		if (it != kvs.end()) {
			// Remove value
			auto &values = it->values;
			auto valueIt = std::ranges::find(values, value);

			if (valueIt != values.end()) {
				values.erase(valueIt);

				// If no values left, remove the key
				if (values.empty()) {
					kvs.erase(it);
				}

				serializeStringKVs(kvs);
				return true;
			}
		}

		return false; // Key or value not found
	}

	std::vector<int64_t> Index::findStringValues(const std::string &key) const {
		if (metadata.nodeType != NodeType::LEAF) {
			return {}; // Only leaf nodes can store values
		}

		auto kvs = deserializeStringKVs();

		// Find key
		auto it = std::ranges::find_if(kvs, [&key](const KeyValuePair &kvp) { return kvp.key == key; });

		if (it != kvs.end()) {
			return it->values;
		}

		return {}; // Key not found
	}

	bool Index::addChild(const std::string &key, int64_t childId) {
		if (metadata.nodeType != NodeType::INTERNAL) {
			return false; // Only internal nodes can have children
		}

		auto children = deserializeChildren();

		// Check if key already exists
		auto it = std::ranges::find_if(children, [&key](const ChildEntry &entry) { return entry.key == key; });

		if (it != children.end()) {
			// Update existing child
			it->childId = childId;
		} else {
			// Add new child
			ChildEntry newEntry{key, childId};

			// Find sorted position
			auto insertPos = std::ranges::lower_bound(
					children, newEntry, [](const ChildEntry &a, const ChildEntry &b) { return a.key < b.key; });

			children.insert(insertPos, newEntry);
		}

		serializeChildren(children);
		return true;
	}

	bool Index::removeChild(const std::string &key) {
		if (metadata.nodeType != NodeType::INTERNAL) {
			return false; // Only internal nodes can have children
		}

		auto children = deserializeChildren();

		// Find key
		auto it = std::ranges::find_if(children, [&key](const ChildEntry &entry) { return entry.key == key; });

		if (it != children.end()) {
			children.erase(it);
			serializeChildren(children);
			return true;
		}

		return false; // Key not found
	}

	int64_t Index::findChild(const std::string &key) const {
		if (metadata.nodeType != NodeType::INTERNAL) {
			return 0; // Only internal nodes can have children
		}

		auto children = deserializeChildren();

		// Find the largest key less than or equal to the search key
		auto it = std::lower_bound(
				children.begin(), children.end(), key,
				[](const ChildEntry &entry, const std::string &searchKey) { return entry.key < searchKey; });

		// If we found an exact match or a key greater than the search key, use it
		if (it != children.end()) {
			if (it->key <= key) {
				return it->childId;
			}

			// If we found a key greater than the search key, use the previous key
			if (it != children.begin()) {
				--it;
				return it->childId;
			}
		}

		// If we're at the beginning, and there are children, use the first child
		if (!children.empty()) {
			return children.front().childId;
		}

		return 0; // No suitable child found
	}

	std::vector<std::pair<std::string, int64_t>> Index::getAllChildren() const {
		if (metadata.nodeType != NodeType::INTERNAL) {
			return {}; // Only internal nodes can have children
		}

		auto children = deserializeChildren();
		std::vector<std::pair<std::string, int64_t>> result;

		result.reserve(children.size());
		for (const auto &entry: children) {
			result.emplace_back(entry.key, entry.childId);
		}

		return result;
	}

} // namespace graph::storage
