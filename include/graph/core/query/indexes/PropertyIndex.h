/**
 * @file PropertyIndex.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <variant>
#include <mutex>
#include <shared_mutex>

namespace graph::query::indexes {

	// Property value can be different types
	using PropertyValue = std::variant<std::string, int64_t, double, bool>;

	class PropertyIndex {
	public:
		PropertyIndex();

		// Add a property to the index
		void addProperty(uint64_t nodeId, const std::string& key, const PropertyValue& value);

		// Remove a property from the index
		void removeProperty(uint64_t nodeId, const std::string& key);

		// Find nodes with a specific property value
		[[nodiscard]] std::vector<uint64_t> findExactMatch(const std::string& key, const PropertyValue& value) const;

		// Find nodes with property values in a range (for numeric properties)
		[[nodiscard]] std::vector<uint64_t> findRange(const std::string& key, double minValue, double maxValue) const;

		// Clear the index
		void clear();

	private:
		// Hash-based exact match index: key -> value -> node IDs
		std::unordered_map<std::string, std::unordered_map<std::string, std::vector<uint64_t>>> stringIndex_;
		std::unordered_map<std::string, std::unordered_map<int64_t, std::vector<uint64_t>>> intIndex_;
		std::unordered_map<std::string, std::unordered_map<double, std::vector<uint64_t>>> doubleIndex_;
		std::unordered_map<std::string, std::unordered_map<bool, std::vector<uint64_t>>> boolIndex_;

		// B-tree style ordered index for range queries: key -> (value -> node IDs)
		std::unordered_map<std::string, std::map<double, std::vector<uint64_t>>> rangeIndex_;

		// Reverse index: nodeId -> key -> value
		std::unordered_map<uint64_t, std::unordered_map<std::string, PropertyValue>> nodeProperties_;

		// Thread safety
		mutable std::shared_mutex mutex_;
	};

} // namespace graph::query::indexes