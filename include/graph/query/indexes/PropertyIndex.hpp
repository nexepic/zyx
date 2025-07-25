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
#include <variant>
#include <vector>
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/data/DataManager.hpp"

#include <graph/core/StateRegistry.hpp>

namespace graph::query::indexes {

	class PropertyIndex {
	public:
		// Constants for this index type
		static constexpr uint32_t PROPERTY_INDEX_TYPE = 2;

		static constexpr char STATE_STRING_ROOTS_KEY[] = "index.property.string_roots";
		static constexpr char STATE_INT_ROOTS_KEY[] = "index.property.int_roots";
		static constexpr char STATE_DOUBLE_ROOTS_KEY[] = "index.property.double_roots";
		static constexpr char STATE_BOOL_ROOTS_KEY[] = "index.property.bool_roots";
		static constexpr char STATE_INDEX_ENABLED_KEY[] = "index.property.enabled";

		explicit PropertyIndex(const std::shared_ptr<storage::DataManager> &dataManager);

		void saveState();

		void clearKey(const std::string &key);
		void dropKey(const std::string &key);

		void initialize();
		void clear();
		void drop();
		void flush();

		// Add property to index
		void addProperty(int64_t nodeId, const std::string &key, const PropertyValue &value);

		// Remove property from index
		void removeProperty(int64_t nodeId, const std::string &key);

		// Find nodes with exact property match
		std::vector<int64_t> findExactMatch(const std::string &key, const PropertyValue &value) const;

		// Find nodes with property value in range (for numeric types)
		std::vector<int64_t> findRange(const std::string &key, double minValue, double maxValue) const;

		// Check if node has property with specific value
		bool hasPropertyValue(int64_t nodeId, const std::string &key, const PropertyValue &value) const;

		std::vector<std::string> getIndexedKeys() const;

		bool isEmpty() const;

		bool hasKeyIndexed(const std::string &key) const {
			std::shared_lock lock(mutex_);
			return stringRoots_.find(key) != stringRoots_.end() || intRoots_.find(key) != intRoots_.end() ||
				   doubleRoots_.find(key) != doubleRoots_.end() || boolRoots_.find(key) != boolRoots_.end();
		}

	private:
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

		// Helper methods
		std::shared_ptr<IndexTreeManager> getTreeManagerForType(const PropertyValue &value) const;
		std::unordered_map<std::string, int64_t> &getRootMapForType(const PropertyValue &value);
		const std::unordered_map<std::string, int64_t> &getRootMapForType(const PropertyValue &value) const;

		// Convert property value to string key for tree storage
		static std::string valueToString(const PropertyValue &value);

		static void serializeRootMap(const std::string &stateKey,
									 const std::unordered_map<std::string, int64_t> &rootMap);
		static std::unordered_map<std::string, int64_t> deserializeRootMap(const std::string &stateKey);
	};

} // namespace graph::query::indexes
