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

#include <functional>
#include <iosfwd>
#include <memory>
#include <vector>
#include "PropertyTypes.hpp"
#include "Types.hpp"
#include "graph/core/Entity.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph {

	class Index : public EntityBase<Index> {
	public:
		// Enum to distinguish between internal nodes and leaf nodes
		enum class NodeType : uint8_t { INTERNAL = 0, LEAF = 1 };

		struct Metadata {
			int64_t id = 0; // Unique ID for this index entity
			int64_t parentId = 0; // Parent node ID (0 if root)
			int64_t nextLeafId = 0; // Next leaf node (for leaf traversal)
			int64_t prevLeafId = 0; // Previous leaf node (for leaf traversal)
			uint32_t indexType = 0; // Type of index (label, property, etc.)
			uint32_t entryCount = 0; // Number of keys in this node
			uint32_t childCount = 0; // Number of children (internal nodes only)
			uint32_t dataUsage = 0; // Tracks the actual bytes used in the dataBuffer. Crucial for underflow checks.
			uint8_t level = 0; // Level in the tree (0 for leaf)
			NodeType nodeType = NodeType::LEAF; // Node type (leaf or internal)
			bool isActive = true; // Whether this node is active
		};

		static constexpr size_t TOTAL_INDEX_SIZE = 256; // Larger size for index nodes
		static constexpr size_t METADATA_SIZE = offsetof(Metadata, isActive) + sizeof(Metadata::isActive);
		static constexpr size_t DATA_SIZE = TOTAL_INDEX_SIZE - METADATA_SIZE;
		static constexpr uint32_t typeId = toUnderlying(EntityType::Index);

		// TODO: Wasting space here, but this is a good compromise for now.
		// Max size for a key to be stored inline within an internal node's ChildEntry.
		static constexpr size_t INTERNAL_KEY_INLINE_THRESHOLD = 32;
		static constexpr size_t LEAF_KEY_INLINE_THRESHOLD = 32;
		static constexpr size_t LEAF_VALUES_INLINE_THRESHOLD = 32;

		// Constructor for a new index entity
		Index(int64_t id, NodeType type, uint32_t indexType);
		Index() = default;

		// Metadata access for CRTP
		[[nodiscard]] const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		[[nodiscard]] bool isUnderflow(double thresholdRatio) const;

		// Specialized getters/setters
		[[nodiscard]] NodeType getNodeType() const { return metadata.nodeType; }
		[[nodiscard]] uint32_t getIndexType() const { return metadata.indexType; }
		[[nodiscard]] bool isLeaf() const { return metadata.nodeType == NodeType::LEAF; }
		[[nodiscard]] uint32_t getEntryCount() const { return metadata.entryCount; }
		[[nodiscard]] uint8_t getLevel() const { return metadata.level; }
		void setLevel(uint8_t level) { metadata.level = level; }

		// Parent links
		[[nodiscard]] int64_t getParentId() const { return metadata.parentId; }
		void setParentId(int64_t id) { metadata.parentId = id; }

		// Leaf node traversal links
		[[nodiscard]] int64_t getNextLeafId() const { return metadata.nextLeafId; }
		[[nodiscard]] int64_t getPrevLeafId() const { return metadata.prevLeafId; }
		void setNextLeafId(int64_t id) { metadata.nextLeafId = id; }
		void setPrevLeafId(int64_t id) { metadata.prevLeafId = id; }

		// For calculating space requirements
		static constexpr size_t getTotalSize() { return TOTAL_INDEX_SIZE; }

		[[nodiscard]] uint32_t getChildCount() const { return metadata.childCount; }

		// Serialization
		void serialize(std::ostream &os) const;
		static Index deserialize(std::istream &is);

		enum class EntrySerializationFlags : uint8_t {
			NONE = 0,
			KEY_IS_BLOB = 1 << 0, // Bit 0 for key
			VALUES_ARE_BLOB = 1 << 1 // Bit 1 for values
		};

		// Entry for leaf nodes. Now includes a blob ID for oversized value lists.
		struct Entry {
			PropertyValue key;
			std::vector<int64_t> values;
			int64_t keyBlobId = 0; // ID of blob for oversized key. 0 if inline.
			int64_t valuesBlobId = 0; // ID of blob chain for oversized value lists. 0 if inline.
		};

		// Entry for internal nodes. Now includes a blob ID for oversized keys.
		struct ChildEntry {
			PropertyValue key; // Separator key for internal nodes
			int64_t childId{};
			int64_t keyBlobId = 0; // ID of blob for oversized key. 0 if inline.
		};

		void insertEntry(const PropertyValue &key, int64_t value,
						 const std::shared_ptr<storage::DataManager> &dataManager,
						 const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator);
		bool removeEntry(const PropertyValue &key, int64_t value,
						 const std::shared_ptr<storage::DataManager> &dataManager,
						 const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator);
		[[nodiscard]] std::vector<int64_t>
		findValues(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
				   const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) const;
		[[nodiscard]] std::vector<Entry> getAllEntries(const std::shared_ptr<storage::DataManager> &dataManager) const;
		void setAllEntries(std::vector<Entry> &entries, const std::shared_ptr<storage::DataManager> &dataManager);
		[[nodiscard]] bool wouldLeafOverflowOnInsert(
				const PropertyValue &key, int64_t value, const std::shared_ptr<storage::DataManager> &dataManager,
				const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) const;


		[[nodiscard]] bool
		wouldInternalOverflowOnAddChild(const ChildEntry &newEntry,
										const std::shared_ptr<storage::DataManager> &dataManager) const;

		void addChild(ChildEntry &newEntry, const std::shared_ptr<storage::DataManager> &dataManager,
					  const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator);
		bool removeChild(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
						 const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator);
		[[nodiscard]] int64_t
		findChild(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
				  const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) const;
		[[nodiscard]] std::vector<ChildEntry>
		getAllChildren(const std::shared_ptr<storage::DataManager> &dataManager) const;
		void setAllChildren(std::vector<ChildEntry> &children,
							const std::shared_ptr<storage::DataManager> &dataManager);

		bool updateChildId(int64_t oldChildId, int64_t newChildId);
		[[nodiscard]] std::vector<int64_t> getChildIds() const;

	private:
		Metadata metadata;
		char dataBuffer[DATA_SIZE] = {}; // Buffer for serialized key-value pairs or child pointers
	};

} // namespace graph
