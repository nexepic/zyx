/**
 * @file PropertyIndex.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
#include "graph/core/IndexTreeManager.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	class PropertyIndex {
	public:
		PropertyIndex(std::shared_ptr<storage::DataManager> &dataManager, uint32_t indexType,
					  const std::string &stateKeyPrefix);

		void saveState() const;

		void clearKey(const std::string &key);
		void dropKey(const std::string &key);

		void initialize();
		void clear();
		void drop();
		void flush() const;

		// Add property to index
		void addProperty(int64_t entityId, const std::string &key, const PropertyValue &value);

		void removeProperty(int64_t entityId, const std::string &key, const PropertyValue &value);

		// Find nodes with exact property match
		std::vector<int64_t> findExactMatch(const std::string &key, const PropertyValue &value) const;

		// Find nodes with property value in range (for numeric types)
		std::vector<int64_t> findRange(const std::string &key, double minValue, double maxValue) const;

		std::vector<std::string> getIndexedKeys() const;

		bool isEmpty() const;

		bool hasKeyIndexed(const std::string &key) const {
			std::shared_lock lock(mutex_);
			return stringRoots_.contains(key) || intRoots_.contains(key) || doubleRoots_.contains(key) ||
				   boolRoots_.contains(key);
		}

		PropertyType getIndexedKeyType(const std::string &key) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;

		const std::string STATE_STRING_ROOTS_KEY;
		const std::string STATE_INT_ROOTS_KEY;
		const std::string STATE_DOUBLE_ROOTS_KEY;
		const std::string STATE_BOOL_ROOTS_KEY;
		const std::string STATE_KEY_TYPES_KEY;

		std::shared_ptr<IndexTreeManager> stringTreeManager_;
		std::shared_ptr<IndexTreeManager> intTreeManager_;
		std::shared_ptr<IndexTreeManager> doubleTreeManager_;
		std::shared_ptr<IndexTreeManager> boolTreeManager_;

		mutable std::shared_mutex mutex_;

		// Root IDs for different property types
		std::unordered_map<std::string, int64_t> stringRoots_;
		std::unordered_map<std::string, int64_t> intRoots_;
		std::unordered_map<std::string, int64_t> doubleRoots_;
		std::unordered_map<std::string, int64_t> boolRoots_;

		// Map to store the determined type for each indexed key.
		std::unordered_map<std::string, PropertyType> indexedKeyTypes_;

		std::shared_ptr<IndexTreeManager> getTreeManagerForType(PropertyType type) const;
		std::unordered_map<std::string, int64_t> &getRootMapForType(PropertyType type);
		const std::unordered_map<std::string, int64_t> &getRootMapForType(PropertyType type) const;

		void serializeRootMap(const std::string &stateKey,
							  const std::unordered_map<std::string, int64_t> &rootMap) const;
		std::unordered_map<std::string, int64_t> deserializeRootMap(const std::string &stateKey);

		void serializeKeyTypeMap() const;
		void deserializeKeyTypeMap();
	};

} // namespace graph::query::indexes
