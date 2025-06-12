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
#include <algorithm>
#include <mutex>

namespace graph::query::indexes {

	PropertyIndex::PropertyIndex() = default;

	void PropertyIndex::addProperty(uint64_t nodeId, const std::string &key, const PropertyValue &value) {
		std::unique_lock lock(mutex_);

		// Remove any existing property with this key for this node
		removeProperty(nodeId, key);

		// Store in the appropriate index based on the value type
		std::visit(
				[&](const auto &v) {
					using T = std::decay_t<decltype(v)>;

					if constexpr (std::is_same_v<T, std::string>) {
						stringIndex_[key][v].push_back(nodeId);
					} else if constexpr (std::is_same_v<T, int64_t>) {
						intIndex_[key][v].push_back(nodeId);
						// Also add to range index for numeric values
						double numericValue = static_cast<double>(v);
						rangeIndex_[key][numericValue].push_back(nodeId);
					} else if constexpr (std::is_same_v<T, double>) {
						doubleIndex_[key][v].push_back(nodeId);
						// Also add to range index
						rangeIndex_[key][v].push_back(nodeId);
					} else if constexpr (std::is_same_v<T, bool>) {
						boolIndex_[key][v].push_back(nodeId);
					}
				},
				value);

		// Update reverse mapping
		nodeProperties_[nodeId][key] = value;
	}

	void PropertyIndex::removeProperty(uint64_t nodeId, const std::string &key) {
		// Note: mutex is expected to be locked by caller

		// Check if the node has this property
		auto nodeIt = nodeProperties_.find(nodeId);
		if (nodeIt == nodeProperties_.end()) {
			return;
		}

		auto propertyIt = nodeIt->second.find(key);
		if (propertyIt == nodeIt->second.end()) {
			return;
		}

		// Remove from the appropriate index based on value type
		std::visit(
				[&](const auto &v) {
					using T = std::decay_t<decltype(v)>;

					if constexpr (std::is_same_v<T, std::string>) {
						auto &nodeIds = stringIndex_[key][v];
						nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());
						if (nodeIds.empty()) {
							stringIndex_[key].erase(v);
							if (stringIndex_[key].empty()) {
								stringIndex_.erase(key);
							}
						}
					} else if constexpr (std::is_same_v<T, int64_t>) {
						// Remove from int index
						auto &nodeIds = intIndex_[key][v];
						nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());
						if (nodeIds.empty()) {
							intIndex_[key].erase(v);
							if (intIndex_[key].empty()) {
								intIndex_.erase(key);
							}
						}

						// Also remove from range index
						double numericValue = static_cast<double>(v);
						auto &rangeNodeIds = rangeIndex_[key][numericValue];
						rangeNodeIds.erase(std::remove(rangeNodeIds.begin(), rangeNodeIds.end(), nodeId),
										   rangeNodeIds.end());
						if (rangeNodeIds.empty()) {
							rangeIndex_[key].erase(numericValue);
							if (rangeIndex_[key].empty()) {
								rangeIndex_.erase(key);
							}
						}
					} else if constexpr (std::is_same_v<T, double>) {
						// Remove from double index
						auto &nodeIds = doubleIndex_[key][v];
						nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());
						if (nodeIds.empty()) {
							doubleIndex_[key].erase(v);
							if (doubleIndex_[key].empty()) {
								doubleIndex_.erase(key);
							}
						}

						// Also remove from range index
						auto &rangeNodeIds = rangeIndex_[key][v];
						rangeNodeIds.erase(std::remove(rangeNodeIds.begin(), rangeNodeIds.end(), nodeId),
										   rangeNodeIds.end());
						if (rangeNodeIds.empty()) {
							rangeIndex_[key].erase(v);
							if (rangeIndex_[key].empty()) {
								rangeIndex_.erase(key);
							}
						}
					} else if constexpr (std::is_same_v<T, bool>) {
						auto &nodeIds = boolIndex_[key][v];
						nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());
						if (nodeIds.empty()) {
							boolIndex_[key].erase(v);
							if (boolIndex_[key].empty()) {
								boolIndex_.erase(key);
							}
						}
					}
				},
				propertyIt->second);

		// Remove from reverse mapping
		nodeIt->second.erase(key);
		if (nodeIt->second.empty()) {
			nodeProperties_.erase(nodeId);
		}
	}

	std::vector<int64_t> PropertyIndex::findExactMatch(const std::string &key, const PropertyValue &value) const {
		std::shared_lock lock(mutex_);

		return std::visit(
				[&](const auto &v) -> std::vector<int64_t> {
					using T = std::decay_t<decltype(v)>;

					if constexpr (std::is_same_v<T, std::string>) {
						auto indexIt = stringIndex_.find(key);
						if (indexIt == stringIndex_.end()) {
							return {};
						}

						auto valueIt = indexIt->second.find(v);
						if (valueIt == indexIt->second.end()) {
							return {};
						}

						return valueIt->second;
					} else if constexpr (std::is_same_v<T, int64_t>) {
						auto indexIt = intIndex_.find(key);
						if (indexIt == intIndex_.end()) {
							return {};
						}

						auto valueIt = indexIt->second.find(v);
						if (valueIt == indexIt->second.end()) {
							return {};
						}

						return valueIt->second;
					} else if constexpr (std::is_same_v<T, double>) {
						auto indexIt = doubleIndex_.find(key);
						if (indexIt == doubleIndex_.end()) {
							return {};
						}

						auto valueIt = indexIt->second.find(v);
						if (valueIt == indexIt->second.end()) {
							return {};
						}

						return valueIt->second;
					} else if constexpr (std::is_same_v<T, bool>) {
						auto indexIt = boolIndex_.find(key);
						if (indexIt == boolIndex_.end()) {
							return {};
						}

						auto valueIt = indexIt->second.find(v);
						if (valueIt == indexIt->second.end()) {
							return {};
						}

						return valueIt->second;
					} else {
						// Unsupported type
						return {};
					}
				},
				value);
	}

	std::vector<int64_t> PropertyIndex::findRange(const std::string &key, double minValue, double maxValue) const {
		std::shared_lock lock(mutex_);

		auto rangeIt = rangeIndex_.find(key);
		if (rangeIt == rangeIndex_.end()) {
			return {};
		}

		std::vector<int64_t> result;

		// Find all values in the range
		auto startIt = rangeIt->second.lower_bound(minValue);
		auto endIt = rangeIt->second.upper_bound(maxValue);

		// Collect all node IDs
		for (auto it = startIt; it != endIt; ++it) {
			const auto &nodeIds = it->second;
			result.insert(result.end(), nodeIds.begin(), nodeIds.end());
		}

		// Remove duplicates if any
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());

		return result;
	}

	void PropertyIndex::clear() {
		std::unique_lock lock(mutex_);

		stringIndex_.clear();
		intIndex_.clear();
		doubleIndex_.clear();
		boolIndex_.clear();
		rangeIndex_.clear();
		nodeProperties_.clear();
	}

} // namespace graph::query::indexes
