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

	PropertyIndex::PropertyIndex(const std::shared_ptr<storage::DataManager> &dataManager, uint32_t indexType,
								 const std::string &stateKeyPrefix) :
		STATE_STRING_ROOTS_KEY(stateKeyPrefix + ".string_roots"), STATE_INT_ROOTS_KEY(stateKeyPrefix + ".int_roots"),
		STATE_DOUBLE_ROOTS_KEY(stateKeyPrefix + ".double_roots"), STATE_BOOL_ROOTS_KEY(stateKeyPrefix + ".bool_roots"),
		STATE_KEY_TYPES_KEY(stateKeyPrefix + ".key_types") {
		stringTreeManager_ = std::make_shared<IndexTreeManager>(dataManager, indexType);
		intTreeManager_ = std::make_shared<IndexTreeManager>(dataManager, indexType);
		doubleTreeManager_ = std::make_shared<IndexTreeManager>(dataManager, indexType);
		boolTreeManager_ = std::make_shared<IndexTreeManager>(dataManager, indexType);
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
			StateRegistry::getDataManager()->removeState(STATE_STRING_ROOTS_KEY);
		}
		if (intRoots_.empty()) {
			StateRegistry::getDataManager()->removeState(STATE_INT_ROOTS_KEY);
		}
		if (doubleRoots_.empty()) {
			StateRegistry::getDataManager()->removeState(STATE_DOUBLE_ROOTS_KEY);
		}
		if (boolRoots_.empty()) {
			StateRegistry::getDataManager()->removeState(STATE_BOOL_ROOTS_KEY);
		}

		if (indexedKeyTypes_.empty()) {
			StateRegistry::getDataManager()->removeState(STATE_KEY_TYPES_KEY);
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
		StateRegistry::getDataManager()->removeState(STATE_STRING_ROOTS_KEY);
		StateRegistry::getDataManager()->removeState(STATE_INT_ROOTS_KEY);
		StateRegistry::getDataManager()->removeState(STATE_DOUBLE_ROOTS_KEY);
		StateRegistry::getDataManager()->removeState(STATE_BOOL_ROOTS_KEY);
		StateRegistry::getDataManager()->removeState(STATE_KEY_TYPES_KEY);
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
		if (valueType == PropertyType::UNKNOWN) {
			return; // Do not index unknown or unsupported types
		}

		auto it = indexedKeyTypes_.find(key);

		if (it == indexedKeyTypes_.end()) {
			// First time seeing this key. Register its type.
			indexedKeyTypes_[key] = valueType;
		} else if (it->second != valueType) {
			// Type mismatch! Do not index this value.
			// In a real system, this should go to a proper logging framework.
			std::cerr << "WARNING: Property key '" << key << "' is indexed as " << static_cast<int>(it->second)
					  << " but received a value of type " << static_cast<int>(valueType) << " for entity " << entityId
					  << ". Value will not be indexed." << std::endl;
			return;
		}

		// Proceed with adding the property to the correct B-Tree
		PropertyType registeredType = indexedKeyTypes_[key];
		auto treeManager = getTreeManagerForType(registeredType);
		auto &rootMap = getRootMapForType(registeredType);

		if (!rootMap.contains(key)) {
			rootMap[key] = treeManager->initialize();
		}

		std::string btreeKey = valueToString(value);
		rootMap[key] = treeManager->insert(rootMap[key], btreeKey, entityId);
	}

	void PropertyIndex::removeProperty(int64_t entityId, const std::string &key, const PropertyValue &value) {
		std::unique_lock lock(mutex_);

		// Find the registered type for this key to know which index to search
		auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			return; // Key is not indexed, nothing to remove
		}

		PropertyType registeredType = it->second;
		PropertyType valueType = getPropertyType(value);

		// Only proceed if the value type matches the indexed type
		if (registeredType != valueType) {
			return;
		}

		auto treeManager = getTreeManagerForType(registeredType);
		auto &rootMap = getRootMapForType(registeredType);
		auto rootIt = rootMap.find(key);

		if (rootIt == rootMap.end()) {
			return; // No root for this key
		}

		int64_t rootId = rootIt->second;
		std::string btreeKey = valueToString(value);
		(void) treeManager->remove(rootId, btreeKey, entityId);
	}

	std::vector<int64_t> PropertyIndex::findExactMatch(const std::string &key, const PropertyValue &value) const {
		std::shared_lock lock(mutex_);

		PropertyType valueType = getPropertyType(value);
		auto it = indexedKeyTypes_.find(key);

		// The key must be indexed and the query value type must match the index type
		if (it == indexedKeyTypes_.end() || it->second != valueType) {
			return {};
		}

		const auto &rootMap = getRootMapForType(valueType);
		auto rootIt = rootMap.find(key);
		if (rootIt == rootMap.end()) {
			return {}; // Should not happen if key is in type map, but good practice
		}

		auto treeManager = getTreeManagerForType(valueType);
		std::string btreeKey = valueToString(value);
		return treeManager->find(rootIt->second, btreeKey);
	}

	std::vector<int64_t> PropertyIndex::findRange(const std::string &key, double minValue, double maxValue) const {
		std::shared_lock lock(mutex_);

		auto it = indexedKeyTypes_.find(key);
		if (it == indexedKeyTypes_.end()) {
			return {}; // Key not indexed
		}

		PropertyType type = it->second;
		std::vector<int64_t> results;

		if (type == PropertyType::INTEGER) {
			auto rootIt = intRoots_.find(key);
			if (rootIt != intRoots_.end()) {
				// Correctly use integer comparison
				auto minKey = std::to_string(static_cast<int64_t>(std::ceil(minValue)));
				auto maxKey = std::to_string(static_cast<int64_t>(std::floor(maxValue)));
				auto rangeResults = intTreeManager_->findRange(rootIt->second, minKey, maxKey);
				results.insert(results.end(), rangeResults.begin(), rangeResults.end());
			}
		} else if (type == PropertyType::DOUBLE) {
			auto rootIt = doubleRoots_.find(key);
			if (rootIt != doubleRoots_.end()) {
				// Correctly use double comparison
				auto minKey = std::to_string(minValue);
				auto maxKey = std::to_string(maxValue);
				auto rangeResults = doubleTreeManager_->findRange(rootIt->second, minKey, maxKey);
				results.insert(results.end(), rangeResults.begin(), rangeResults.end());
			}
		}
		// Note: Range queries on other types are not supported.

		return results;
	}

	PropertyType PropertyIndex::getPropertyType(const PropertyValue &value) {
		if (std::holds_alternative<std::string>(value))
			return PropertyType::STRING;
		if (std::holds_alternative<int64_t>(value))
			return PropertyType::INTEGER;
		if (std::holds_alternative<double>(value))
			return PropertyType::DOUBLE;
		if (std::holds_alternative<bool>(value))
			return PropertyType::BOOLEAN;
		return PropertyType::UNKNOWN;
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

	std::string PropertyIndex::valueToString(const PropertyValue &value) {
		if (std::holds_alternative<std::string>(value)) {
			return std::get<std::string>(value);
		} else if (std::holds_alternative<int64_t>(value)) {
			return std::to_string(std::get<int64_t>(value));
		} else if (std::holds_alternative<double>(value)) {
			return std::to_string(std::get<double>(value));
		} else if (std::holds_alternative<bool>(value)) {
			return std::get<bool>(value) ? "true" : "false";
		}
		return ""; // Should never reach here
	}

	void PropertyIndex::serializeRootMap(const std::string &stateKey,
										 const std::unordered_map<std::string, int64_t> &rootMap) {
		// Convert root map to properties
		std::unordered_map<std::string, PropertyValue> properties;

		for (const auto &[key, rootId]: rootMap) {
			properties[key] = rootId;
		}

		// Store in state entity
		StateRegistry::getDataManager()->addStateProperties(stateKey, properties);
	}

	std::unordered_map<std::string, int64_t> PropertyIndex::deserializeRootMap(const std::string &stateKey) {
		std::unordered_map<std::string, int64_t> rootMap;

		// Get properties from state entity
		auto properties = StateRegistry::getDataManager()->getStateProperties(stateKey);

		// Convert properties to root map
		for (const auto &[key, value]: properties) {
			if (std::holds_alternative<int64_t>(value)) {
				rootMap[key] = std::get<int64_t>(value);
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
		StateRegistry::getDataManager()->addStateProperties(STATE_KEY_TYPES_KEY, properties);
	}

	void PropertyIndex::deserializeKeyTypeMap() {
		indexedKeyTypes_.clear();
		auto properties = StateRegistry::getDataManager()->getStateProperties(STATE_KEY_TYPES_KEY);
		for (const auto &[key, value]: properties) {
			if (std::holds_alternative<int64_t>(value)) {
				indexedKeyTypes_[key] = static_cast<PropertyType>(std::get<int64_t>(value));
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
