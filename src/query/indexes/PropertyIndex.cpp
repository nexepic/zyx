/**
 * @file PropertyIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/PropertyIndex.hpp"
#include <cmath>
#include <ranges>
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::indexes {

	PropertyIndex::PropertyIndex(std::shared_ptr<storage::DataManager> &dataManager, uint32_t indexType,
								 const std::string &stateKeyPrefix) :
		dataManager_(std::move(dataManager)), STATE_STRING_ROOTS_KEY(stateKeyPrefix + ".string_roots"),
		STATE_INT_ROOTS_KEY(stateKeyPrefix + ".int_roots"), STATE_DOUBLE_ROOTS_KEY(stateKeyPrefix + ".double_roots"),
		STATE_BOOL_ROOTS_KEY(stateKeyPrefix + ".bool_roots"), STATE_KEY_TYPES_KEY(stateKeyPrefix + ".key_types") {
		stringTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::STRING);
		intTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::INTEGER);
		doubleTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::DOUBLE);
		boolTreeManager_ = std::make_shared<IndexTreeManager>(dataManager_, indexType, PropertyType::BOOLEAN);
		initialize();
	}

	void PropertyIndex::initialize() {
		std::unique_lock lock(mutex_);
		// Load root maps from state using the constructed keys
		stringRoots_ = deserializeRootMap(STATE_STRING_ROOTS_KEY);
		intRoots_ = deserializeRootMap(STATE_INT_ROOTS_KEY);
		doubleRoots_ = deserializeRootMap(STATE_DOUBLE_ROOTS_KEY);
		boolRoots_ = deserializeRootMap(STATE_BOOL_ROOTS_KEY);
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
			dataManager_->removeState(STATE_STRING_ROOTS_KEY);
		}
		if (intRoots_.empty()) {
			dataManager_->removeState(STATE_INT_ROOTS_KEY);
		}
		if (doubleRoots_.empty()) {
			dataManager_->removeState(STATE_DOUBLE_ROOTS_KEY);
		}
		if (boolRoots_.empty()) {
			dataManager_->removeState(STATE_BOOL_ROOTS_KEY);
		}

		if (indexedKeyTypes_.empty()) {
			dataManager_->removeState(STATE_KEY_TYPES_KEY);
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
		dataManager_->removeState(STATE_STRING_ROOTS_KEY);
		dataManager_->removeState(STATE_INT_ROOTS_KEY);
		dataManager_->removeState(STATE_DOUBLE_ROOTS_KEY);
		dataManager_->removeState(STATE_BOOL_ROOTS_KEY);
		dataManager_->removeState(STATE_KEY_TYPES_KEY);
	}

	void PropertyIndex::flush() const {
		// Save current state to persistent storage
		saveState();
	}

	void PropertyIndex::saveState() const {
		std::shared_lock lock(mutex_);

		// Save all root maps to state
		if (!stringRoots_.empty()) {
			serializeRootMap(STATE_STRING_ROOTS_KEY, stringRoots_);
		}
		if (!intRoots_.empty()) {
			serializeRootMap(STATE_INT_ROOTS_KEY, intRoots_);
		}
		if (!doubleRoots_.empty()) {
			serializeRootMap(STATE_DOUBLE_ROOTS_KEY, doubleRoots_);
		}
		if (!boolRoots_.empty()) {
			serializeRootMap(STATE_BOOL_ROOTS_KEY, boolRoots_);
		}

		// Save the key->type map
		if (!indexedKeyTypes_.empty()) {
			serializeKeyTypeMap();
		}
	}

	void PropertyIndex::addProperty(int64_t entityId, const std::string &key, const PropertyValue &value) {
		std::unique_lock lock(mutex_);

		PropertyType valueType = getPropertyType(value);
		if (valueType == PropertyType::UNKNOWN || valueType == PropertyType::NULL_TYPE) {
			return;
		}

		auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			indexedKeyTypes_[key] = valueType;
		} else if (it->second != valueType) {
			// Type mismatch warning remains the same
			return;
		}

		auto treeManager = getTreeManagerForType(valueType);
		auto &rootMap = getRootMapForType(valueType);

		if (!rootMap.contains(key)) {
			rootMap[key] = treeManager->initialize();
		}

		// Pass the PropertyValue directly to the manager's insert method.
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

	void PropertyIndex::serializeRootMap(const std::string &stateKey,
										 const std::unordered_map<std::string, int64_t> &rootMap) const {
		// Convert root map to properties
		std::unordered_map<std::string, PropertyValue> properties;

		for (const auto &[key, rootId]: rootMap) {
			properties[key] = rootId;
		}

		// Store in state entity
		dataManager_->addStateProperties(stateKey, properties);
	}

	std::unordered_map<std::string, int64_t> PropertyIndex::deserializeRootMap(const std::string &stateKey) {
		std::unordered_map<std::string, int64_t> rootMap;
		auto properties = dataManager_->getStateProperties(stateKey);

		for (const auto &[key, value]: properties) {
			// Correctly access the variant inside the wrapper struct
			if (const auto *val_ptr = std::get_if<int64_t>(&value.getVariant())) {
				rootMap[key] = *val_ptr;
			}
		}
		return rootMap;
	}

	void PropertyIndex::serializeKeyTypeMap() const {
		std::unordered_map<std::string, PropertyValue> properties;
		for (const auto &[key, type]: indexedKeyTypes_) {
			// Store the enum value as an integer (int64_t)
			properties[key] = static_cast<int64_t>(type);
		}
		dataManager_->addStateProperties(STATE_KEY_TYPES_KEY, properties);
	}

	void PropertyIndex::deserializeKeyTypeMap() {
		indexedKeyTypes_.clear();
		auto properties = dataManager_->getStateProperties(STATE_KEY_TYPES_KEY);
		for (const auto &[key, value]: properties) {
			// Use the safe std::get_if pattern here as well
			if (const auto *val_ptr = std::get_if<int64_t>(&value.getVariant())) {
				indexedKeyTypes_[key] = static_cast<PropertyType>(*val_ptr);
			}
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
