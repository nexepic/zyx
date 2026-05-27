#include "graph/query/execution/NodePropertyColumnLoader.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		bool isSelectedRow(const std::vector<uint8_t> &selected, size_t row) {
			return selected.empty() || selected[row] != 0;
		}

		std::vector<std::string> deduplicateProperties(const std::vector<std::string> &requiredProperties) {
			std::vector<std::string> keys;
			keys.reserve(requiredProperties.size());
			for (const auto &key : requiredProperties) {
				if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
					keys.push_back(key);
				}
			}
			return keys;
		}

		void assignRequestedValues(
				std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
				const std::vector<std::string> &requestedProperties,
				size_t row,
				const std::unordered_map<std::string, PropertyValue> &values) {
			for (const auto &key : requestedProperties) {
				auto valueIt = values.find(key);
				if (valueIt != values.end()) {
					columns[key][row] = valueIt->second;
				}
			}
		}
	} // namespace

	NodePropertyColumnLoader::NodePropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
	                                                 concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	NodePropertyColumnLoader::loadColumns(const std::vector<Node> &nodes,
	                                      const std::vector<uint8_t> &selected,
	                                      const std::vector<std::string> &requiredProperties) const {
		std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
		if (requiredProperties.empty()) {
			return columns;
		}
		if (!selected.empty() && selected.size() != nodes.size()) {
			return columns;
		}

		const auto requestedProperties = deduplicateProperties(requiredProperties);
		for (const auto &key : requestedProperties) {
			columns.emplace(key, std::vector<std::optional<PropertyValue>>(nodes.size(), std::nullopt));
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto extractStart = traceEnabled ? Clock::now() : Clock::time_point{};

		std::vector<int64_t> propertyEntityIds;
		std::vector<size_t> externalRows;
		std::vector<size_t> fallbackRows;
		propertyEntityIds.reserve(nodes.size());
		externalRows.reserve(nodes.size());
		fallbackRows.reserve(nodes.size());

		for (size_t row = 0; row < nodes.size(); ++row) {
			const auto &node = nodes[row];
			if (!isSelectedRow(selected, row) || node.getId() == 0 || !node.isActive()) {
				continue;
			}

			assignRequestedValues(columns, requestedProperties, row, node.getProperties());

			if (!node.hasPropertyEntity()) {
				continue;
			}

			if (node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
				propertyEntityIds.push_back(node.getPropertyEntityId());
				externalRows.push_back(row);
			} else if (node.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
				fallbackRows.push_back(row);
			}
		}

		std::sort(propertyEntityIds.begin(), propertyEntityIds.end());
		propertyEntityIds.erase(std::unique(propertyEntityIds.begin(), propertyEntityIds.end()), propertyEntityIds.end());

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.extract_property_columns", elapsedNs(extractStart));
		}

		std::unordered_map<int64_t, Property> propertyMap;
		if (dm_ && !propertyEntityIds.empty()) {
			const auto bulkStart = traceEnabled ? Clock::now() : Clock::time_point{};
			propertyMap = dm_->bulkLoadPropertyEntities(propertyEntityIds, threadPool_);
			if (traceEnabled) {
				debug::PerfTrace::addDuration("node_scan.load_property_entities", elapsedNs(bulkStart));
			}
		}

		const auto fillStart = traceEnabled ? Clock::now() : Clock::time_point{};
		for (const size_t row : externalRows) {
			const auto &node = nodes[row];
			auto propIt = propertyMap.find(node.getPropertyEntityId());
			if (propIt != propertyMap.end() && propIt->second.getId() != 0 && propIt->second.isActive()) {
				assignRequestedValues(columns, requestedProperties, row, propIt->second.getPropertyValues());
			} else {
				fallbackRows.push_back(row);
			}
		}

		if (dm_) {
			std::sort(fallbackRows.begin(), fallbackRows.end());
			fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
			for (const size_t row : fallbackRows) {
				assignRequestedValues(columns, requestedProperties, row, dm_->getNodePropertiesDirect(nodes[row]));
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.extract_property_columns", elapsedNs(fillStart));
		}

		return columns;
	}

} // namespace graph::query::execution
