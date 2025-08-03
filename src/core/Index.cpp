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
#include <graph/storage/data/DataManager.hpp>
#include <sstream>
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

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

	void Index::storeDataInBlob(const std::shared_ptr<storage::DataManager> &dataManager, const std::string &data) {
		if (!dataManager) {
			throw std::runtime_error("DataManager is required for blob storage operations");
		}
		if (hasBlobStorage()) {
			dataManager->getBlobManager()->deleteBlobChain(getBlobId());
		}
		const auto blobChain = dataManager->getBlobManager()->createBlobChain(getId(), typeId, data);
		if (blobChain.empty()) {
			throw std::runtime_error("Failed to create blob chain for index data");
		}
		setBlobId(blobChain[0].getId());
		setDataStorageType(DataStorageType::BLOB_CHAIN);
		std::memset(dataBuffer, 0, DATA_SIZE);
	}

	std::string Index::retrieveDataFromBlob(const std::shared_ptr<storage::DataManager> &dataManager) const {
		if (!hasBlobStorage()) {
			return {dataBuffer, DATA_SIZE};
		}
		if (!dataManager) {
			throw std::runtime_error("DataManager is required to retrieve blob data");
		}
		return dataManager->getBlobManager()->readBlobChain(getBlobId());
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
	std::vector<Index::KeyValuePair>
	Index::getAllKeyValues(const std::shared_ptr<storage::DataManager> &dataManager) const {
		if (metadata.nodeType != NodeType::LEAF) {
			throw std::logic_error("Key-values can only be retrieved from leaf nodes.");
		}

		std::string serializedData;
		if (hasBlobStorage()) {
			serializedData = retrieveDataFromBlob(dataManager);
		} else {
			serializedData = std::string(dataBuffer, DATA_SIZE);
		}

		std::vector<KeyValuePair> result;
		if (metadata.keyCount == 0)
			return result;

		result.reserve(metadata.keyCount);
		std::istringstream is(serializedData);

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

	// Update setAllKeyValues to handle blob storage
	void Index::setAllKeyValues(const std::vector<KeyValuePair> &keyValues,
								const std::shared_ptr<storage::DataManager> &dataManager) {
		if (metadata.nodeType != NodeType::LEAF) {
			throw std::logic_error("Key-values can only be set on leaf nodes.");
		}

		std::ostringstream os;
		for (const auto &kvp: keyValues) {
			utils::Serializer::writeString(os, kvp.key);
			utils::Serializer::writePOD(os, static_cast<uint32_t>(kvp.values.size()));
			for (int64_t value: kvp.values) {
				utils::Serializer::writePOD(os, value);
			}
		}
		std::string data = os.str();
		metadata.keyCount = static_cast<uint32_t>(keyValues.size());

		// Clean up old blob if it exists, as the new data might fit inline
		if (hasBlobStorage()) {
			if (!dataManager)
				throw std::runtime_error("DataManager is required to clean up existing blob storage.");
			dataManager->getBlobManager()->deleteBlobChain(getBlobId());
			setBlobId(0); // Invalidate blob ID before deciding new storage type
		}

		if (data.size() <= DATA_SIZE) {
			setDataStorageType(DataStorageType::INLINE);
			std::memset(dataBuffer, 0, DATA_SIZE);
			std::memcpy(dataBuffer, data.data(), data.size());
		} else {
			if (!dataManager) {
				throw std::runtime_error("Data size exceeds buffer and DataManager is not available for blob storage");
			}
			storeDataInBlob(dataManager, data);
		}
	}

	void Index::insertStringKey(const std::string &key, int64_t value,
								const std::shared_ptr<storage::DataManager> &dataManager) {
		auto kvs = getAllKeyValues(dataManager);
		auto it = std::lower_bound(kvs.begin(), kvs.end(), key,
								   [](const KeyValuePair &a, const std::string &b) { return a.key < b; });

		if (it != kvs.end() && it->key == key) {
			if (std::find(it->values.begin(), it->values.end(), value) == it->values.end()) {
				it->values.push_back(value);
			} else {
				return; // Value already exists
			}
		} else {
			kvs.insert(it, {key, {value}});
		}
		setAllKeyValues(kvs, dataManager);
	}

	bool Index::removeStringKey(const std::string &key, int64_t value,
								const std::shared_ptr<storage::DataManager> &dataManager) {
		auto kvs = getAllKeyValues(dataManager);
		auto it = std::find_if(kvs.begin(), kvs.end(), [&](const KeyValuePair &kvp) { return kvp.key == key; });

		if (it != kvs.end()) {
			auto &values = it->values;
			auto value_it = std::find(values.begin(), values.end(), value);
			if (value_it != values.end()) {
				values.erase(value_it);
				if (values.empty()) {
					kvs.erase(it);
				}
				setAllKeyValues(kvs, dataManager);
				return true;
			}
		}
		return false;
	}

	std::vector<int64_t> Index::findStringValues(const std::string &key,
												 const std::shared_ptr<storage::DataManager> &dataManager) const {
		auto kvs = getAllKeyValues(dataManager);
		auto it = std::find_if(kvs.cbegin(), kvs.cend(), [&](const KeyValuePair &kvp) { return kvp.key == key; });
		return (it != kvs.cend()) ? it->values : std::vector<int64_t>{};
	}

	void Index::addChild(const std::string &key, int64_t childId) {
		auto children = getAllChildren();
		auto it = std::lower_bound(children.begin(), children.end(), key,
								   [](const ChildEntry &a, const std::string &b) { return a.key < b; });

		if (it != children.end() && it->key == key) {
			it->childId = childId; // Update existing
		} else {
			children.insert(it, {key, childId}); // Add new
		}
		setAllChildren(children);
	}

	bool Index::removeChild(const std::string &key) {
		auto children = getAllChildren();
		auto it = std::find_if(children.begin(), children.end(), [&](const ChildEntry &e) { return e.key == key; });

		if (it != children.end()) {
			children.erase(it);
			setAllChildren(children);
			return true;
		}
		return false;
	}

	int64_t Index::findChild(const std::string &key) const {
		auto children = getAllChildren();
		if (children.empty())
			return 0;

		// Find the first element whose key is GREATER than the search key
		auto it = std::upper_bound(children.begin(), children.end(), key,
								   [](const std::string &k, const ChildEntry &entry) {
									   // An empty key in the tree acts as -infinity, so any search key is >= it.
									   if (entry.key.empty())
										   return false;
									   return k < entry.key;
								   });

		// The correct child is pointed to by the key just BEFORE this one.
		// If `it` is the beginning, it means `key` is smaller than all keys,
		// so we should descend into the first child.
		if (it == children.begin()) {
			return children.front().childId;
		}

		// Otherwise, the correct child is the one before `it`.
		return std::prev(it)->childId;
	}

	std::vector<Index::ChildEntry> Index::getAllChildren() const {
		if (metadata.nodeType != NodeType::INTERNAL) {
			throw std::logic_error("Children can only be retrieved from internal nodes.");
		}
		std::vector<ChildEntry> result;
		if (metadata.childCount == 0)
			return result;

		std::istringstream is(std::string(dataBuffer, DATA_SIZE));
		result.reserve(metadata.childCount);
		for (uint32_t i = 0; i < metadata.childCount; i++) {
			result.push_back({utils::Serializer::readString(is), utils::Serializer::readPOD<int64_t>(is)});
		}
		return result;
	}

	void Index::setAllChildren(const std::vector<ChildEntry> &children) {
		if (metadata.nodeType != NodeType::INTERNAL) {
			throw std::logic_error("Children can only be set on internal nodes.");
		}
		std::ostringstream os;
		for (const auto &entry: children) {
			utils::Serializer::writeString(os, entry.key);
			utils::Serializer::writePOD(os, entry.childId);
		}
		std::string data = os.str();
		if (data.size() > DATA_SIZE) {
			// This signals the manager to split the internal node.
			throw std::runtime_error("Child entries exceed node capacity.");
		}
		metadata.childCount = static_cast<uint32_t>(children.size());
		std::memset(dataBuffer, 0, DATA_SIZE);
		std::memcpy(dataBuffer, data.data(), data.size());
	}

} // namespace graph
