#include "graph/query/execution/RelationshipPropertyColumnLoader.hpp"

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

		void appendRowsMissingFromBulkLoad(const std::vector<size_t> &externalRows,
		                                   std::vector<size_t> loadedRows,
		                                   std::vector<size_t> &fallbackRows) {
			std::sort(loadedRows.begin(), loadedRows.end());
			loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
			for (const size_t row : externalRows) {
				if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) {
					fallbackRows.push_back(row);
				}
			}
		}
	} // namespace

	RelationshipPropertyColumnLoader::RelationshipPropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
	                                                                 concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	RelationshipPropertyColumnLoader::loadColumns(const std::vector<Edge> &edges,
	                                             const std::vector<uint8_t> &selected,
	                                             const std::vector<std::string> &requiredProperties) const {
		std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
		if (requiredProperties.empty()) {
			return columns;
		}
		if (!selected.empty() && selected.size() != edges.size()) {
			return columns;
		}

		const auto requestedProperties = deduplicateProperties(requiredProperties);
		for (const auto &key : requestedProperties) {
			columns.emplace(key, std::vector<std::optional<PropertyValue>>(edges.size(), std::nullopt));
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto extractStart = traceEnabled ? Clock::now() : Clock::time_point{};

		std::vector<int64_t> propertyEntityIds;
		std::vector<size_t> externalRows;
		std::vector<size_t> fallbackRows;
		propertyEntityIds.reserve(edges.size());
		externalRows.reserve(edges.size());
		fallbackRows.reserve(edges.size());

		for (size_t row = 0; row < edges.size(); ++row) {
			const auto &edge = edges[row];
			if (!isSelectedRow(selected, row) || edge.getId() == 0 || !edge.isActive()) {
				continue;
			}

			assignRequestedValues(columns, requestedProperties, row, edge.getProperties());

			if (!edge.hasPropertyEntity()) {
				continue;
			}

			if (edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
				propertyEntityIds.push_back(edge.getPropertyEntityId());
				externalRows.push_back(row);
			} else if (edge.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
				fallbackRows.push_back(row);
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.extract_property_columns", elapsedNs(extractStart));
		}

		std::vector<size_t> loadedRows;
		if (dm_ && !propertyEntityIds.empty()) {
			const auto bulkStart = traceEnabled ? Clock::now() : Clock::time_point{};
			loadedRows = dm_->bulkLoadPropertyEntityColumns(
				propertyEntityIds, externalRows, edges.size(), requestedProperties, columns, threadPool_);
			if (traceEnabled) {
				debug::PerfTrace::addDuration("relationship_count.load_property_entities", elapsedNs(bulkStart));
			}
		}

		const auto fillStart = traceEnabled ? Clock::now() : Clock::time_point{};
		appendRowsMissingFromBulkLoad(externalRows, std::move(loadedRows), fallbackRows);

		if (dm_) {
			std::sort(fallbackRows.begin(), fallbackRows.end());
			fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
			for (const size_t row : fallbackRows) {
				assignRequestedValues(columns, requestedProperties, row, dm_->getEdgeProperties(edges[row].getId()));
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.extract_property_columns", elapsedNs(fillStart));
		}

		return columns;
	}

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
	RelationshipPropertyColumnLoader::loadColumns(const RelationshipMetadataBatch &metadataBatch,
	                                             const std::vector<uint8_t> &selected,
	                                             const std::vector<std::string> &requiredProperties) const {
		std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
		if (requiredProperties.empty()) {
			return columns;
		}
		if (!selected.empty() && selected.size() != metadataBatch.size()) {
			return columns;
		}

		const auto requestedProperties = deduplicateProperties(requiredProperties);
		for (const auto &key : requestedProperties) {
			columns.emplace(key, std::vector<std::optional<PropertyValue>>(metadataBatch.size(), std::nullopt));
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto extractStart = traceEnabled ? Clock::now() : Clock::time_point{};

		std::vector<int64_t> propertyEntityIds;
		std::vector<size_t> externalRows;
		std::vector<size_t> fallbackRows;
		propertyEntityIds.reserve(metadataBatch.size());
		externalRows.reserve(metadataBatch.size());
		fallbackRows.reserve(metadataBatch.size());

		for (size_t row = 0; row < metadataBatch.size(); ++row) {
			if (!isSelectedRow(selected, row) || !metadataBatch.isValid(row) || metadataBatch.active[row] == 0) {
				continue;
			}

			const auto storageType = metadataBatch.propertyStorageTypes[row];
			const int64_t propertyEntityId = metadataBatch.propertyEntityIds[row];
			if (storageType == PropertyStorageType::PROPERTY_ENTITY && propertyEntityId != 0) {
				propertyEntityIds.push_back(propertyEntityId);
				externalRows.push_back(row);
			} else if (storageType == PropertyStorageType::BLOB_ENTITY && propertyEntityId != 0) {
				fallbackRows.push_back(row);
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.extract_property_columns", elapsedNs(extractStart));
		}

		std::vector<size_t> loadedRows;
		if (dm_ && !propertyEntityIds.empty()) {
			const auto bulkStart = traceEnabled ? Clock::now() : Clock::time_point{};
			loadedRows = dm_->bulkLoadPropertyEntityColumns(
				propertyEntityIds, externalRows, metadataBatch.size(), requestedProperties, columns, threadPool_);
			if (traceEnabled) {
				debug::PerfTrace::addDuration("relationship_count.load_property_entities", elapsedNs(bulkStart));
			}
		}

		const auto fillStart = traceEnabled ? Clock::now() : Clock::time_point{};
		appendRowsMissingFromBulkLoad(externalRows, std::move(loadedRows), fallbackRows);

		if (dm_) {
			std::sort(fallbackRows.begin(), fallbackRows.end());
			fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
			for (const size_t row : fallbackRows) {
				assignRequestedValues(columns, requestedProperties, row, dm_->getEdgeProperties(metadataBatch.edgeIds[row]));
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.extract_property_columns", elapsedNs(fillStart));
		}

		return columns;
	}

} // namespace graph::query::execution
