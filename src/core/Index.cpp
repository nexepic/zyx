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
#include <sstream>
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	Index::Index(const int64_t id, NodeType type, const uint32_t indexType) {
		metadata.id = id;
		metadata.nodeType = type;
		metadata.indexType = indexType;
		metadata.entryCount = 0;
		metadata.childCount = 0;
		metadata.dataUsage = 0;
		metadata.level = (type == NodeType::LEAF) ? 0 : 1;
	}

	bool Index::isUnderflow(double thresholdRatio) const {
		// A node is in underflow if its data usage is less than the specified ratio of its capacity.
		// We don't check for underflow on the root, as it has different fill rules.
		if (metadata.parentId == 0) {
			return false;
		}
		return metadata.dataUsage < static_cast<uint32_t>(static_cast<double>(DATA_SIZE) * thresholdRatio);
	}

	inline Index::EntrySerializationFlags operator|(Index::EntrySerializationFlags a,
													Index::EntrySerializationFlags b) {
		return static_cast<Index::EntrySerializationFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
	}

	inline bool operator&(Index::EntrySerializationFlags a, Index::EntrySerializationFlags b) {
		return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
	}

	void Index::serialize(std::ostream &os) const {
		// The order must match deserialize
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.parentId);
		utils::Serializer::writePOD(os, metadata.nextLeafId);
		utils::Serializer::writePOD(os, metadata.prevLeafId);
		utils::Serializer::writePOD(os, metadata.indexType);
		utils::Serializer::writePOD(os, metadata.entryCount);
		utils::Serializer::writePOD(os, metadata.childCount);
		utils::Serializer::writePOD(os, metadata.dataUsage);
		utils::Serializer::writePOD(os, metadata.level);
		utils::Serializer::writePOD(os, static_cast<uint8_t>(metadata.nodeType));
		utils::Serializer::writePOD(os, metadata.isActive);

		os.write(dataBuffer, DATA_SIZE);
	}

	Index Index::deserialize(std::istream &is) {
		Index entity;
		entity.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.parentId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.nextLeafId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.prevLeafId = utils::Serializer::readPOD<int64_t>(is);
		entity.metadata.indexType = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.entryCount = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.childCount = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.dataUsage = utils::Serializer::readPOD<uint32_t>(is);
		entity.metadata.level = utils::Serializer::readPOD<uint8_t>(is);
		entity.metadata.nodeType = static_cast<NodeType>(utils::Serializer::readPOD<uint8_t>(is));
		entity.metadata.isActive = utils::Serializer::readPOD<bool>(is);

		is.read(entity.dataBuffer, DATA_SIZE);
		return entity;
	}

	std::vector<Index::Entry> Index::getAllEntries(const std::shared_ptr<storage::DataManager> &dataManager) const {
		if (metadata.nodeType != NodeType::LEAF) {
			throw std::logic_error("Entries can only be retrieved from leaf nodes.");
		}
		std::vector<Entry> result;
		if (metadata.entryCount == 0)
			return result;

		std::string serializedData(dataBuffer, metadata.dataUsage); // Use dataUsage for accurate length
		std::istringstream is(serializedData);
		result.reserve(metadata.entryCount);

		for (uint32_t i = 0; i < metadata.entryCount; i++) {
			Entry entry;
			// Read the flags byte first to determine the structure.
			auto flags = utils::Serializer::readPOD<EntrySerializationFlags>(is);

			// Read key from blob or inline based on the flag.
			if (flags & EntrySerializationFlags::KEY_IS_BLOB) {
				entry.keyBlobId = utils::Serializer::readPOD<int64_t>(is);
				if (!dataManager)
					throw std::runtime_error("DataManager required to retrieve key blob data.");
				std::string keyData = dataManager->getBlobManager()->readBlobChain(entry.keyBlobId);
				std::istringstream keyStream(keyData);
				entry.key = utils::Serializer::deserialize<PropertyValue>(keyStream);
			} else {
				entry.keyBlobId = 0; // Ensure blob ID is 0 if not used.
				entry.key = utils::Serializer::deserialize<PropertyValue>(is);
			}

			// Read value count, which is always present.
			auto valueCount = utils::Serializer::readPOD<uint32_t>(is);
			entry.values.reserve(valueCount);

			// Read values from blob or inline based on the flag.
			if (flags & EntrySerializationFlags::VALUES_ARE_BLOB) {
				entry.valuesBlobId = utils::Serializer::readPOD<int64_t>(is);
				if (!dataManager)
					throw std::runtime_error("DataManager required to retrieve value blob data.");
				std::string blobData = dataManager->getBlobManager()->readBlobChain(entry.valuesBlobId);
				std::istringstream valueStream(blobData);
				for (uint32_t j = 0; j < valueCount; ++j) {
					entry.values.push_back(utils::Serializer::readPOD<int64_t>(valueStream));
				}
			} else {
				entry.valuesBlobId = 0; // Ensure blob ID is 0 if not used.
				for (uint32_t j = 0; j < valueCount; j++) {
					entry.values.push_back(utils::Serializer::readPOD<int64_t>(is));
				}
			}
			result.push_back(std::move(entry));
		}
		return result;
	}

	void Index::setAllEntries(std::vector<Entry> &entries, const std::shared_ptr<storage::DataManager> &dataManager) {
		if (!isLeaf())
			throw std::logic_error("Entries can only be set on leaf nodes.");

		std::ostringstream os;
		for (auto &entry: entries) {
			// --- Determine if blobs are needed and manage them ---
			const bool shouldKeyUseBlob = utils::getSerializedSize(entry.key) > LEAF_KEY_INLINE_THRESHOLD;
			const bool shouldValuesUseBlob = (entry.values.size() * sizeof(int64_t)) > LEAF_VALUES_INLINE_THRESHOLD;

			// Cleanup or create key blob if necessary
			if (entry.keyBlobId != 0 && !shouldKeyUseBlob) {
				dataManager->getBlobManager()->deleteBlobChain(entry.keyBlobId);
				entry.keyBlobId = 0;
			}
			if (shouldKeyUseBlob) {
				if (!dataManager)
					throw std::runtime_error("DataManager not available for key blob storage.");
				std::ostringstream keyStream;
				utils::Serializer::serialize(keyStream, entry.key);
				if (entry.keyBlobId != 0) {
					auto blob = dataManager->getBlobManager()->updateBlobChain(entry.keyBlobId, getId(), typeId,
																			   keyStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to update blob chain for entry key.");
					entry.keyBlobId = blob[0].getId();
				} else {
					auto blob = dataManager->getBlobManager()->createBlobChain(getId(), typeId, keyStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to create blob chain for entry key.");
					entry.keyBlobId = blob[0].getId();
				}
			}

			// Cleanup or create values blob if necessary
			if (entry.valuesBlobId != 0 && !shouldValuesUseBlob) {
				dataManager->getBlobManager()->deleteBlobChain(entry.valuesBlobId);
				entry.valuesBlobId = 0;
			}
			if (shouldValuesUseBlob) {
				if (!dataManager)
					throw std::runtime_error("DataManager not available for value blob storage.");
				std::ostringstream valueStream;
				for (int64_t val: entry.values)
					utils::Serializer::writePOD(valueStream, val);
				if (entry.valuesBlobId != 0) {
					auto blob = dataManager->getBlobManager()->updateBlobChain(entry.valuesBlobId, getId(), typeId,
																			   valueStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to update blob chain for entry values.");
					entry.valuesBlobId = blob[0].getId();
				} else {
					auto blob = dataManager->getBlobManager()->createBlobChain(getId(), typeId, valueStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to create blob chain for entry values.");
					entry.valuesBlobId = blob[0].getId();
				}
			}

			// --- Serialize entry using the flags byte ---
			auto flags = EntrySerializationFlags::NONE;
			if (shouldKeyUseBlob)
				flags = flags | EntrySerializationFlags::KEY_IS_BLOB;
			if (shouldValuesUseBlob)
				flags = flags | EntrySerializationFlags::VALUES_ARE_BLOB;
			utils::Serializer::writePOD(os, flags);

			// Write key (or its blob ID)
			if (shouldKeyUseBlob) {
				utils::Serializer::writePOD(os, entry.keyBlobId);
			} else {
				utils::Serializer::serialize(os, entry.key);
			}

			// Always write value count
			utils::Serializer::writePOD(os, static_cast<uint32_t>(entry.values.size()));

			// Write values (or their blob ID)
			if (shouldValuesUseBlob) {
				utils::Serializer::writePOD(os, entry.valuesBlobId);
			} else {
				for (int64_t value: entry.values) {
					utils::Serializer::writePOD(os, value);
				}
			}
		}

		std::string data = os.str();
		if (data.size() > DATA_SIZE) {
			throw std::runtime_error(
					"FATAL: Leaf node data exceeds capacity after handling overflows. A split should have occurred.");
		}

		metadata.entryCount = static_cast<uint32_t>(entries.size());
		metadata.dataUsage = static_cast<uint32_t>(data.size());
		std::memset(dataBuffer, 0, DATA_SIZE);
		std::memcpy(dataBuffer, data.data(), data.size());
	}

	/**
	 * @brief Estimates the serialized size of all children to check for potential overflow.
	 * This calculation must exactly match the serialization logic in setAllChildren.
	 */
	bool Index::wouldInternalOverflowOnAddChild(const ChildEntry &newEntry,
												const std::shared_ptr<storage::DataManager> &dataManager) const {
		if (isLeaf()) {
			return false;
		}

		auto children = getAllChildren(dataManager);
		children.push_back(newEntry); // Simulate the addition.

		// Now serialize exactly as setAllChildren would
		std::ostringstream os;

		// The first child is always just its ID.
		if (!children.empty()) {
			utils::Serializer::writePOD(os, children[0].childId);
		}

		// For each subsequent child, we serialize a flag, the key (or blob ID), and the child ID.
		for (size_t i = 1; i < children.size(); ++i) {
			const auto &entry = children[i];
			auto flags = EntrySerializationFlags::NONE;

			// Check if key should use blob
			const bool keyIsBlob = utils::getSerializedSize(entry.key) > INTERNAL_KEY_INLINE_THRESHOLD;
			if (keyIsBlob) {
				flags = flags | EntrySerializationFlags::KEY_IS_BLOB;
			}

			utils::Serializer::writePOD(os, flags);

			if (keyIsBlob) {
				utils::Serializer::writePOD(os, entry.keyBlobId);
			} else {
				utils::Serializer::serialize(os, entry.key);
			}

			utils::Serializer::writePOD(os, entry.childId);
		}

		return os.str().size() > DATA_SIZE;
	}

	Index::InsertResult Index::tryInsertEntry(const PropertyValue &key, int64_t value,
                         const std::shared_ptr<storage::DataManager> &dataManager,
                         const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
        if (!isLeaf()) {
            throw std::logic_error("Keys can only be inserted into leaf nodes.");
        }

        // 1. DESERIALIZE ONCE
        auto entries = getAllEntries(dataManager);

        // 2. INSERT IN MEMORY
        auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                   [&](const Entry &a, const PropertyValue &b) { return comparator(a.key, b); });

        bool keyExists = (it != entries.end() && !comparator(key, it->key) && !comparator(it->key, key));

        if (keyExists) {
            auto &values = it->values;
            // Optimization: Avoid duplicate value check if we trust the caller,
            // but std::find is safer for consistency.
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            } else {
                // Value already exists, nothing changed.
                // We can strictly say "Success" without serialization overhead if we tracked dirtiness,
                // but for now, we just return success.
                return {true, {}};
            }
        } else {
            entries.insert(it, {key, {value}, 0, 0});
        }

        // 3. CALCULATE SIZE (Simulate Serialization)
        // We calculate the size accurately. If it fits, we serialize for real.
        size_t estimatedSize = 0;
        for (const auto &entry : entries) {
            estimatedSize += getEntrySerializedSize(entry);
        }

        // 4. COMMIT OR RETURN OVERFLOW
        if (estimatedSize <= DATA_SIZE) {
            // Fits! Serialize back to dataBuffer.
            setAllEntries(entries, dataManager);
            return {true, {}};
        } else {
            // Overflow! Return the modified list so the caller can split it.
            // We DO NOT modify 'this' node's buffer yet.
            return {false, std::move(entries)};
        }
    }

	void Index::insertEntry(const PropertyValue &key, int64_t value,
							const std::shared_ptr<storage::DataManager> &dataManager,
							const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
		if (!isLeaf())
			throw std::logic_error("Keys can only be inserted into leaf nodes.");
		auto entries = getAllEntries(dataManager);
		auto it = std::lower_bound(entries.begin(), entries.end(), key,
								   [&](const Entry &a, const PropertyValue &b) { return comparator(a.key, b); });

		bool keyExists = (it != entries.end() && !comparator(key, it->key) && !comparator(it->key, key));

		if (keyExists) {
			auto &values = it->values;
			if (std::find(values.begin(), values.end(), value) == values.end()) {
				values.push_back(value);
			}
		} else {
			entries.insert(it, {key, {value}, 0, 0});
		}
		setAllEntries(entries, dataManager);
	}

	bool Index::removeEntry(const PropertyValue &key, int64_t value,
							const std::shared_ptr<storage::DataManager> &dataManager,
							const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
		auto entries = getAllEntries(dataManager);
		auto it = std::lower_bound(entries.begin(), entries.end(), key,
								   [&](const Entry &a, const PropertyValue &b) { return comparator(a.key, b); });

		if (it != entries.end() && !comparator(key, it->key) && !comparator(it->key, key)) {
			auto &values = it->values;
			auto value_it = std::remove(values.begin(), values.end(), value);
			if (value_it != values.end()) {
				values.erase(value_it, values.end());
				if (values.empty()) {
					// If the entry is now empty, delete associated blobs before erasing.
					if (!dataManager)
						throw std::runtime_error("DataManager required to clean up blobs.");
					if (it->valuesBlobId != 0) {
						dataManager->getBlobManager()->deleteBlobChain(it->valuesBlobId);
					}
					if (it->keyBlobId != 0) {
						dataManager->getBlobManager()->deleteBlobChain(it->keyBlobId);
					}
					entries.erase(it);
				}
				setAllEntries(entries, dataManager);
				return true;
			}
		}
		return false;
	}

	std::vector<int64_t>
	Index::findValues(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
					  const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) const {
		auto entries = getAllEntries(dataManager);
		auto it = std::lower_bound(entries.begin(), entries.end(), key,
								   [&](const Entry &a, const PropertyValue &b) { return comparator(a.key, b); });

		bool keyExists = (it != entries.end() && !comparator(key, it->key) && !comparator(it->key, key));

		return keyExists ? it->values : std::vector<int64_t>{};
	}

	void Index::addChild(ChildEntry &newEntry, const std::shared_ptr<storage::DataManager> &dataManager,
						 const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
		if (isLeaf())
			throw std::logic_error("Children can only be added to internal nodes.");
		auto children = getAllChildren(dataManager);

		auto it = std::lower_bound(children.begin() + 1, children.end(), newEntry.key,
								   [&](const ChildEntry &a, const PropertyValue &b) { return comparator(a.key, b); });

		children.insert(it, newEntry);
		setAllChildren(children, dataManager);
	}

	bool Index::removeChild(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
							const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) {
		if (isLeaf())
			throw std::logic_error("Children can only be removed from internal nodes.");
		auto children = getAllChildren(dataManager);

		auto it = std::lower_bound(children.begin() + 1, children.end(), key,
								   [&](const ChildEntry &a, const PropertyValue &b) { return comparator(a.key, b); });

		if (it != children.end() && !comparator(key, it->key) && !comparator(it->key, key)) {
			if (it->keyBlobId != 0) {
				if (!dataManager)
					throw std::runtime_error("DataManager required to clean up blob.");
				dataManager->getBlobManager()->deleteBlobChain(it->keyBlobId);
			}
			children.erase(it);
			setAllChildren(children, dataManager);
			return true;
		}
		return false;
	}

	int64_t
	Index::findChild(const PropertyValue &key, const std::shared_ptr<storage::DataManager> &dataManager,
					 const std::function<bool(const PropertyValue &, const PropertyValue &)> &comparator) const {
		if (isLeaf())
			throw std::logic_error("findChild is for internal nodes only.");
		auto children = getAllChildren(dataManager);
		if (children.empty())
			return 0;

		auto it = std::upper_bound(children.begin() + 1, children.end(), key,
								   [&](const PropertyValue &searchKey, const ChildEntry &entry) {
									   return comparator(searchKey, entry.key);
								   });

		return std::prev(it)->childId;
	}

	/**
	 * @brief Deserializes all child entries from the internal node's data buffer.
	 * The format is: [childId_0][flags_1][key_1][childId_1][flags_2][key_2][childId_2]...
	 */
	std::vector<Index::ChildEntry>
	Index::getAllChildren(const std::shared_ptr<storage::DataManager> &dataManager) const {
		if (isLeaf()) {
			throw std::logic_error("Children can only be retrieved from internal nodes.");
		}
		std::vector<ChildEntry> result;
		if (metadata.childCount == 0) {
			return result;
		}

		std::string serializedData(dataBuffer, metadata.dataUsage); // Use dataUsage for accurate length
		std::istringstream is(serializedData);
		result.reserve(metadata.childCount);

		// The first child is always stored as just an ID, with an implicit key.
		if (metadata.childCount > 0) {
			ChildEntry firstChild;
			firstChild.childId = utils::Serializer::readPOD<int64_t>(is);
			firstChild.key = PropertyValue(std::monostate{}); // Implicit "negative infinity" key.
			firstChild.keyBlobId = 0;
			result.push_back(std::move(firstChild));
		}

		// Subsequent children are each preceded by a separator key and a flag.
		for (uint32_t i = 1; i < metadata.childCount; i++) {
			ChildEntry entry;
			auto flags = utils::Serializer::readPOD<EntrySerializationFlags>(is);

			if (flags & EntrySerializationFlags::KEY_IS_BLOB) {
				entry.keyBlobId = utils::Serializer::readPOD<int64_t>(is);
				if (!dataManager) {
					throw std::runtime_error("DataManager is required to retrieve blob data.");
				}
				std::string keyData = dataManager->getBlobManager()->readBlobChain(entry.keyBlobId);
				std::istringstream keyStream(keyData);
				entry.key = utils::Serializer::deserialize<PropertyValue>(keyStream);
			} else {
				entry.keyBlobId = 0;
				entry.key = utils::Serializer::deserialize<PropertyValue>(is);
			}
			entry.childId = utils::Serializer::readPOD<int64_t>(is);
			result.push_back(std::move(entry));
		}
		return result;
	}

	/**
	 * @brief Serializes a vector of child entries into the node's data buffer.
	 * The format is: [childId_0][flags_1][key_1][childId_1][flags_2][key_2][childId_2]...
	 */
	void Index::setAllChildren(std::vector<ChildEntry> &children,
							   const std::shared_ptr<storage::DataManager> &dataManager) {
		if (isLeaf()) {
			throw std::logic_error("Children can only be set on internal nodes.");
		}

		std::ostringstream os;

		// Handle the first child pointer, which has no preceding key or flag.
		if (!children.empty()) {
			utils::Serializer::writePOD(os, children[0].childId);
		}

		// Handle subsequent children, each preceded by a separator key and a flag.
		for (size_t i = 1; i < children.size(); ++i) {
			auto &entry = children[i];
			const bool shouldUseBlob = utils::getSerializedSize(entry.key) > INTERNAL_KEY_INLINE_THRESHOLD;

			// --- Blob Management for the key ---
			if (entry.keyBlobId != 0 && !shouldUseBlob) {
				if (!dataManager)
					throw std::runtime_error("DataManager required to clean up blob.");
				dataManager->getBlobManager()->deleteBlobChain(entry.keyBlobId);
				entry.keyBlobId = 0;
			}
			if (shouldUseBlob) {
				if (!dataManager)
					throw std::runtime_error("DataManager not available for blob storage.");
				std::ostringstream keyStream;
				utils::Serializer::serialize(keyStream, entry.key);
				if (entry.keyBlobId != 0) {
					auto blob = dataManager->getBlobManager()->updateBlobChain(entry.keyBlobId, getId(), typeId,
																			   keyStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to update blob chain for entry key.");
					entry.keyBlobId = blob[0].getId();
				} else {
					auto blob = dataManager->getBlobManager()->createBlobChain(getId(), typeId, keyStream.str());
					if (blob.empty())
						throw std::runtime_error("Failed to create blob chain for key.");
					entry.keyBlobId = blob[0].getId();
				}
			}

			// --- Serialization ---
			// The flag only indicates if the key is a blob; values are not a concept here.
			auto flags = shouldUseBlob ? EntrySerializationFlags::KEY_IS_BLOB : EntrySerializationFlags::NONE;
			utils::Serializer::writePOD(os, flags);

			if (shouldUseBlob) {
				utils::Serializer::writePOD(os, entry.keyBlobId);
			} else {
				utils::Serializer::serialize(os, entry.key);
			}
			utils::Serializer::writePOD(os, entry.childId);
		}

		std::string data = os.str();
		if (data.size() > DATA_SIZE) {
			throw std::runtime_error("FATAL: Internal node data exceeds capacity. A split should have been performed.");
		}

		metadata.childCount = static_cast<uint32_t>(children.size());
		metadata.entryCount = (!children.empty()) ? static_cast<uint32_t>(children.size() - 1) : 0;
		metadata.dataUsage = static_cast<uint32_t>(data.size());
		std::memset(dataBuffer, 0, DATA_SIZE);
		std::memcpy(dataBuffer, data.data(), data.size());
	}

	size_t Index::getEntrySerializedSize(const Entry &entry) {
		size_t size = 0;

		// 1. Flags byte
		size += sizeof(uint8_t); // EntrySerializationFlags

		// 2. Key Size
		size_t keySize = graph::utils::getSerializedSize(entry.key);

		if (keySize > LEAF_KEY_INLINE_THRESHOLD) {
			size += sizeof(int64_t); // Blob ID
		} else {
			size += keySize;
		}

		// 3. Value Count
		size += sizeof(uint32_t);

		// 4. Values Size
		size_t valuesRawSize = entry.values.size() * sizeof(int64_t);

		if (valuesRawSize > LEAF_VALUES_INLINE_THRESHOLD) {
			size += sizeof(int64_t); // Blob ID
		} else {
			size += valuesRawSize;
		}

		return size;
	}

	bool Index::updateChildId(int64_t oldChildId, int64_t newChildId) {
		if (isLeaf())
			return false;
		if (metadata.childCount == 0)
			return false;

		std::string currentData(dataBuffer, metadata.dataUsage);
		std::istringstream is(currentData);
		std::ostringstream os; // We will rebuild the buffer

		bool found = false;

		// 1. Process First Child ID (It has no key preceding it)
		{
			auto id = utils::Serializer::readPOD<int64_t>(is);
			if (id == oldChildId) {
				id = newChildId;
				found = true;
			}
			utils::Serializer::writePOD(os, id);
		}

		// 2. Process Remaining Children
		for (uint32_t i = 1; i < metadata.childCount; ++i) {
			// Read Flags
			auto flags = utils::Serializer::readPOD<EntrySerializationFlags>(is);
			utils::Serializer::writePOD(os, flags);

			// Copy Key (Blob ID or Inline)
			if (flags & EntrySerializationFlags::KEY_IS_BLOB) {
				// Blob ID is int64
				auto blobId = utils::Serializer::readPOD<int64_t>(is);
				utils::Serializer::writePOD(os, blobId);
			} else {
				// Inline Key: Deserialize and re-serialize to copy correctly
				// PropertyValue serialization handles variable length strings etc.
				auto key = utils::Serializer::deserialize<PropertyValue>(is);
				utils::Serializer::serialize(os, key);
			}

			// Read and Update Child ID
			auto id = utils::Serializer::readPOD<int64_t>(is);
			if (id == oldChildId) {
				id = newChildId;
				found = true;
			}
			utils::Serializer::writePOD(os, id);
		}

		if (found) {
			std::string newData = os.str();
			if (newData.size() > DATA_SIZE) {
				// This theoretically shouldn't happen as we are just replacing int64 with int64
				throw std::runtime_error("Buffer overflow during child ID update");
			}
			std::memset(dataBuffer, 0, DATA_SIZE);
			std::memcpy(dataBuffer, newData.data(), newData.size());
			// dataUsage remains the same size-wise, but good practice to update if logic changed
			metadata.dataUsage = static_cast<uint32_t>(newData.size());
		}

		return found;
	}

	std::vector<int64_t> Index::getChildIds() const {
		if (isLeaf())
			return {};
		if (metadata.childCount == 0)
			return {};

		std::vector<int64_t> ids;
		ids.reserve(metadata.childCount);

		std::string currentData(dataBuffer, metadata.dataUsage);
		std::istringstream is(currentData);

		// 1. First Child
		ids.push_back(utils::Serializer::readPOD<int64_t>(is));

		// 2. Rest
		for (uint32_t i = 1; i < metadata.childCount; ++i) {
			// Skip Flags
			auto flags = utils::Serializer::readPOD<EntrySerializationFlags>(is);

			// Skip Key
			if (flags & EntrySerializationFlags::KEY_IS_BLOB) {
				is.ignore(sizeof(int64_t)); // Skip Blob ID
			} else {
				// To skip inline PropertyValue properly, we must deserialize it
				// because it has variable length (e.g. strings)
				utils::Serializer::deserialize<PropertyValue>(is);
			}

			// Read Child ID
			ids.push_back(utils::Serializer::readPOD<int64_t>(is));
		}

		return ids;
	}

} // namespace graph
