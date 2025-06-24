/**
 * @file Index.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
#include "Types.hpp"

namespace graph::storage {
	class DataManager;

	class Index {
	public:
		// Enum to distinguish between internal nodes and leaf nodes
		enum class NodeType : uint8_t { INTERNAL = 0, LEAF = 1 };

		enum class DataStorageType : uint8_t {
			INLINE, // Data stored in dataBuffer
			BLOB_CHAIN // Data stored in blob chain
		};

		struct Metadata {
			int64_t id = 0; // Unique ID for this index entity
			int64_t parentId = 0; // Parent node ID (0 if root)
			int64_t nextLeafId = 0; // Next leaf node (for leaf traversal)
			int64_t prevLeafId = 0; // Previous leaf node (for leaf traversal)
			int64_t blobId = 0; // ID of the head blob if using blob storage
			uint32_t indexType = 0; // Type of index (label, property, etc.)
			uint32_t keyCount = 0; // Number of keys in this node
			uint32_t childCount = 0; // Number of children (internal nodes only)
			uint8_t level = 0; // Level in the tree (0 for leaf)
			NodeType nodeType = NodeType::LEAF; // Node type (leaf or internal)
			DataStorageType dataStorageType = DataStorageType::INLINE;
			bool isActive = true; // Whether this node is active
		};

		static constexpr size_t TOTAL_INDEX_SIZE = 256; // Larger size for index nodes
		static constexpr size_t METADATA_SIZE = sizeof(Metadata);
		static constexpr size_t DATA_SIZE = TOTAL_INDEX_SIZE - METADATA_SIZE;

		// For entity type system
		static constexpr uint32_t typeId = toUnderlying(EntityType::Index);

		// Constructor for a new index entity
		Index(int64_t id, NodeType type, uint32_t indexType);
		Index() = default;

		// Basic getters/setters for metadata
		int64_t getId() const { return metadata.id; }
		void setId(int64_t newId) { metadata.id = newId; }
		NodeType getNodeType() const { return metadata.nodeType; }
		uint32_t getIndexType() const { return metadata.indexType; }
		bool isLeaf() const { return metadata.nodeType == NodeType::LEAF; }
		bool isActive() const { return metadata.isActive; }
		void markInactive() { metadata.isActive = false; }
		uint32_t getKeyCount() const { return metadata.keyCount; }
		uint8_t getLevel() const { return metadata.level; }
		void setLevel(uint8_t level) { metadata.level = level; }

		// Parent links
		int64_t getParentId() const { return metadata.parentId; }
		void setParentId(int64_t id) { metadata.parentId = id; }

		// Leaf node traversal links
		int64_t getNextLeafId() const { return metadata.nextLeafId; }
		int64_t getPrevLeafId() const { return metadata.prevLeafId; }
		void setNextLeafId(int64_t id) { metadata.nextLeafId = id; }
		void setPrevLeafId(int64_t id) { metadata.prevLeafId = id; }

		// For checking if temporary
		bool hasTemporaryId() const;
		void setPermanentId(int64_t permanentId);

		// For serialization
		void serialize(std::ostream &os) const;
		static Index deserialize(std::istream &is);

		// For B+Tree operations with strings as keys (for label index)
		bool insertStringKey(const std::string &key, int64_t value);
		bool removeStringKey(const std::string &key, int64_t value);
		std::vector<int64_t> findStringValues(const std::string &key) const;

		// For internal nodes to manage child pointers
		bool addChild(const std::string &key, int64_t childId);
		bool removeChild(const std::string &key);
		int64_t findChild(const std::string &key) const;
		std::vector<std::pair<std::string, int64_t>> getAllChildren() const;

		// For calculating space requirements
		static constexpr size_t getTotalSize() { return TOTAL_INDEX_SIZE; }

		uint32_t getChildCount() const { return metadata.childCount; }

		// Internal helpers for string key operations
		struct KeyValuePair {
			std::string key;
			std::vector<int64_t> values;
		};

		std::vector<KeyValuePair> getAllKeyValues(std::shared_ptr<DataManager> dataManager) const;
		void setAllKeyValues(const std::vector<KeyValuePair> &keyValues, std::shared_ptr<DataManager> dataManager);

		std::vector<KeyValuePair> deserializeStringKVs() const;
		void serializeStringKVs(const std::vector<KeyValuePair> &kvs);

		void setDataStorageType(DataStorageType type);
		DataStorageType getDataStorageType() const;
		void setBlobId(int64_t blobId);
		int64_t getBlobId() const;
		bool hasBlobStorage() const;

		void storeDataInBlob(std::shared_ptr<DataManager> dataManager, const std::string &data);
		std::string retrieveDataFromBlob(std::shared_ptr<DataManager> dataManager) const;

	private:
		Metadata metadata;
		char dataBuffer[DATA_SIZE] = {}; // Buffer for serialized key-value pairs or child pointers

		struct ChildEntry {
			std::string key;
			int64_t childId;
		};

		std::vector<ChildEntry> deserializeChildren() const;
		void serializeChildren(const std::vector<ChildEntry> &children);
	};

} // namespace graph::storage
