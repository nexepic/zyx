/**
 * @file PropertyIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/indexes/PropertyIndex.hpp"
#include <cmath>
#include <ranges>
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::query::indexes {

	PropertyIndex::PropertyIndex(std::shared_ptr<storage::DataManager> &dataManager,
								 std::shared_ptr<storage::state::SystemStateManager> systemStateManager,
								 uint32_t indexType, std::string baseStateKey) :
		dataManager_(std::move(dataManager)), systemStateManager_(std::move(systemStateManager)),
		baseStateKey_(std::move(baseStateKey)) {
		stringTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::STRING);
		intTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::INTEGER);
		doubleTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::DOUBLE);
		boolTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::BOOLEAN);
		initialize();
	}

	void PropertyIndex::initialize() {
		std::unique_lock lock(mutex_);
		// Load root maps from state using the constructed keys
		deserializeRootMap();
		deserializeKeyTypeMap();
	}

	// Clear a specific property key from all type indexes
	void PropertyIndex::clearKey(const std::string &key) {
		std::unique_lock lock(mutex_);

		auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			return; // Key not indexed
		}

		PropertyType type = it->second;
		auto &rootMap = getRootMapForType(type);
		auto rootIt = rootMap.find(key);

		if (rootIt != rootMap.end()) {
			auto treeManager = getTreeManagerForType(type);
			treeManager->clear(rootIt->second);
			rootMap.erase(rootIt);
		}

		// Remove from the type map
		indexedKeyTypes_.erase(it);
	}

	void PropertyIndex::dropKey(const std::string &key) {
		// Clear the specific key from all type indexes
		clearKey(key);

		// Check if all root maps are empty before removing state
		if (stringRoots_.empty()) {
			systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_STRING_ROOTS);
		}
		if (intRoots_.empty()) {
			systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_INT_ROOTS);
		}
		if (doubleRoots_.empty()) {
			systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_DOUBLE_ROOTS);
		}
		if (boolRoots_.empty()) {
			systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_BOOL_ROOTS);
		}

		if (indexedKeyTypes_.empty()) {
			systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_KEY_TYPES);
		}
	}

	void PropertyIndex::clear() {
		std::unique_lock lock(mutex_);

		auto clearAllRoots = [&](auto &rootMap, auto &treeManager) {
			for (const auto &rootId: rootMap | std::views::values) {
				treeManager->clear(rootId);
			}
			rootMap.clear();
		};

		clearAllRoots(stringRoots_, stringTreeManager_);
		clearAllRoots(intRoots_, intTreeManager_);
		clearAllRoots(doubleRoots_, doubleTreeManager_);
		clearAllRoots(boolRoots_, boolTreeManager_);

		// Also clear the type map
		indexedKeyTypes_.clear();
	}

	void PropertyIndex::drop() {
		// Clear all data
		clear();

		// Remove all state entries related to this index using the constructed keys
		systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_STRING_ROOTS);
		systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_INT_ROOTS);
		systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_DOUBLE_ROOTS);
		systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_BOOL_ROOTS);

		systemStateManager_->remove(baseStateKey_ + storage::state::keys::SUFFIX_KEY_TYPES);
	}

	void PropertyIndex::flush() const {
		// Save current state to persistent storage
		saveState();
	}

	void PropertyIndex::createIndex(const std::string &key) {
		std::unique_lock lock(mutex_);
		// If it doesn't exist, register it with UNKNOWN type.
		// If it exists, we leave it alone (preserving existing type info).
		if (!indexedKeyTypes_.contains(key)) {
			indexedKeyTypes_[key] = PropertyType::UNKNOWN;
		}
	}

	void PropertyIndex::clearIndexData(const std::string &key) {
		std::unique_lock lock(mutex_);

		// We do NOT remove from indexedKeyTypes_. We only clear the trees.
		if (!indexedKeyTypes_.contains(key)) {
			return;
		}

		// Clear roots for all potential types associated with this key
		// (Though logically one key usually maps to one type, we clear all to be safe during rebuild)
		auto clearRoot = [&](auto &rootMap, auto &treeManager) {
			if (auto rootIt = rootMap.find(key); rootIt != rootMap.end()) {
				treeManager->clear(rootIt->second);
				rootMap.erase(rootIt);
			}
		};

		clearRoot(stringRoots_, stringTreeManager_);
		clearRoot(intRoots_, intTreeManager_);
		clearRoot(doubleRoots_, doubleTreeManager_);
		clearRoot(boolRoots_, boolTreeManager_);

		// Reset type to UNKNOWN so it can be re-determined during rebuild
		indexedKeyTypes_[key] = PropertyType::UNKNOWN;
	}

	void PropertyIndex::saveState() const {
		std::shared_lock lock(mutex_);

		// Save all root maps to state
		serializeRootMap();

		// Save the key->type map
		serializeKeyTypeMap();
	}

	void PropertyIndex::addProperty(int64_t entityId, const std::string &key, const PropertyValue &value) {
		std::unique_lock lock(mutex_);

		PropertyType valueType = getPropertyType(value);
		if (valueType == PropertyType::UNKNOWN || valueType == PropertyType::NULL_TYPE) {
			return;
		}

		auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			// Auto-creation policy: if not registered, register now.
			indexedKeyTypes_[key] = valueType;
		} else if (it->second == PropertyType::UNKNOWN) {
			// Transition from empty/unknown to concrete type
			it->second = valueType;
		} else if (it->second != valueType) {
			// Type mismatch: In a strict schema, throw. In flexible, maybe ignore or log.
			// For now, we return as per original logic.
			return;
		}

		auto treeManager = getTreeManagerForType(valueType);
		auto &rootMap = getRootMapForType(valueType);

		if (!rootMap.contains(key)) {
			rootMap[key] = treeManager->initialize();
		}

		rootMap[key] = treeManager->insert(rootMap[key], value, entityId);
	}

	void PropertyIndex::removeProperty(int64_t entityId, const std::string &key, const PropertyValue &value) {
		std::unique_lock lock(mutex_);

		// Find the registered type for this key to know which index to search
		const auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			return; // Key is not indexed, nothing to remove
		}

		const PropertyType registeredType = it->second;
		const PropertyType valueType = getPropertyType(value);

		// Only proceed if the value type matches the indexed type
		if (registeredType != valueType) {
			return;
		}

		const auto treeManager = getTreeManagerForType(registeredType);
		auto &rootMap = getRootMapForType(registeredType);
		const auto rootIt = rootMap.find(key);

		if (rootIt == rootMap.end()) {
			return; // No root for this key
		}

		(void) treeManager->remove(rootMap.at(key), value, entityId);
	}

	std::vector<int64_t> PropertyIndex::findExactMatch(const std::string &key, const PropertyValue &value) const {
		std::shared_lock lock(mutex_);
		const PropertyType valueType = getPropertyType(value);
		if (const auto it = indexedKeyTypes_.find(key); it == indexedKeyTypes_.end() || it->second != valueType)
			return {};

		const auto &rootMap = getRootMapForType(valueType);
		const auto rootIt = rootMap.find(key);
		if (rootIt == rootMap.end())
			return {};

		// Pass the PropertyValue directly
		return getTreeManagerForType(valueType)->find(rootIt->second, value);
	}

	std::vector<int64_t> PropertyIndex::findRange(const std::string &key, double minValue, double maxValue) const {
		std::shared_lock lock(mutex_);
		// ... logic to find key type remains ...

		PropertyType type = getIndexedKeyType(key);
		if (type != PropertyType::INTEGER && type != PropertyType::DOUBLE)
			return {};

		const auto &rootMap = getRootMapForType(type);
		auto rootIt = rootMap.find(key);
		if (rootIt == rootMap.end())
			return {};

		PropertyValue minKey, maxKey;
		if (type == PropertyType::INTEGER) {
			minKey = static_cast<int64_t>(std::ceil(minValue));
			maxKey = static_cast<int64_t>(std::floor(maxValue));
		} else {
			minKey = minValue;
			maxKey = maxValue;
		}

		return getTreeManagerForType(type)->findRange(rootIt->second, minKey, maxKey);
	}

	std::shared_ptr<IndexTreeManager> PropertyIndex::getTreeManagerForType(PropertyType type) const {
		switch (type) {
			case PropertyType::STRING:
				return stringTreeManager_;
			case PropertyType::INTEGER:
				return intTreeManager_;
			case PropertyType::DOUBLE:
				return doubleTreeManager_;
			case PropertyType::BOOLEAN:
				return boolTreeManager_;
			default:
				return nullptr;
		}
	}

	std::unordered_map<std::string, int64_t> &PropertyIndex::getRootMapForType(PropertyType type) {
		switch (type) {
			case PropertyType::STRING:
				return stringRoots_;
			case PropertyType::INTEGER:
				return intRoots_;
			case PropertyType::DOUBLE:
				return doubleRoots_;
			case PropertyType::BOOLEAN:
				return boolRoots_;
			default:
				throw std::invalid_argument("Invalid property type for root map");
		}
	}

	const std::unordered_map<std::string, int64_t> &PropertyIndex::getRootMapForType(PropertyType type) const {
		switch (type) {
			case PropertyType::STRING:
				return stringRoots_;
			case PropertyType::INTEGER:
				return intRoots_;
			case PropertyType::DOUBLE:
				return doubleRoots_;
			case PropertyType::BOOLEAN:
				return boolRoots_;
			default:
				throw std::invalid_argument("Invalid property type for root map");
		}
	}

	void PropertyIndex::serializeRootMap() const {
		if (!stringRoots_.empty()) {
			systemStateManager_->setMap(baseStateKey_ + storage::state::keys::SUFFIX_STRING_ROOTS, stringRoots_);
		}
		if (!intRoots_.empty()) {
			systemStateManager_->setMap(baseStateKey_ + storage::state::keys::SUFFIX_INT_ROOTS, intRoots_);
		}
		if (!doubleRoots_.empty()) {
			systemStateManager_->setMap(baseStateKey_ + storage::state::keys::SUFFIX_DOUBLE_ROOTS, doubleRoots_);
		}
		if (!boolRoots_.empty()) {
			systemStateManager_->setMap(baseStateKey_ + storage::state::keys::SUFFIX_BOOL_ROOTS, boolRoots_);
		}
	}

	void PropertyIndex::deserializeRootMap() {
		stringRoots_ = systemStateManager_->getMap<int64_t>(baseStateKey_ + storage::state::keys::SUFFIX_STRING_ROOTS);
		intRoots_ = systemStateManager_->getMap<int64_t>(baseStateKey_ + storage::state::keys::SUFFIX_INT_ROOTS);
		doubleRoots_ = systemStateManager_->getMap<int64_t>(baseStateKey_ + storage::state::keys::SUFFIX_DOUBLE_ROOTS);
		boolRoots_ = systemStateManager_->getMap<int64_t>(baseStateKey_ + storage::state::keys::SUFFIX_BOOL_ROOTS);
	}

	void PropertyIndex::serializeKeyTypeMap() const {
		if (!indexedKeyTypes_.empty()) {
			std::unordered_map<std::string, int64_t> rawTypeMap;
			for (const auto &[k, v]: indexedKeyTypes_) {
				rawTypeMap[k] = static_cast<int64_t>(v);
			}
			systemStateManager_->setMap(baseStateKey_ + storage::state::keys::SUFFIX_KEY_TYPES, rawTypeMap);
		}
	}

	void PropertyIndex::deserializeKeyTypeMap() {
		auto rawTypeMap = systemStateManager_->getMap<int64_t>(baseStateKey_ + storage::state::keys::SUFFIX_KEY_TYPES);
		indexedKeyTypes_.clear();
		for (const auto &[k, v]: rawTypeMap) {
			indexedKeyTypes_[k] = static_cast<PropertyType>(v);
		}
	}

	bool PropertyIndex::isEmpty() const {
		std::shared_lock lock(mutex_);
		// The definitive source of whether an index exists for any key is the type map.
		return indexedKeyTypes_.empty();
	}

	std::vector<std::string> PropertyIndex::getIndexedKeys() const {
		std::shared_lock lock(mutex_);
		std::vector<std::string> keys;
		keys.reserve(indexedKeyTypes_.size());
		for (const auto &key: indexedKeyTypes_ | std::views::keys) {
			keys.push_back(key);
		}
		return keys;
	}

	PropertyType PropertyIndex::getIndexedKeyType(const std::string &key) const {
		std::shared_lock lock(mutex_);
		if (auto it = indexedKeyTypes_.find(key); it != indexedKeyTypes_.end()) {
			return it->second;
		}
		return PropertyType::UNKNOWN;
	}

} // namespace graph::query::indexes
