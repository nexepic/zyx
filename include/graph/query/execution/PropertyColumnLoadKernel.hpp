#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class PropertyColumnLoadKernel {
	public:
		using ColumnMap = std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>;

		struct RowView {
			bool active = false;
			PropertyStorageType storageType = PropertyStorageType::NONE;
			int64_t propertyEntityId = 0;
			const std::unordered_map<std::string, PropertyValue> *inlineProperties = nullptr;
		};

		struct PhaseNames {
			std::string_view extract;
			std::string_view loadPropertyEntities;
		};

		template<typename RowReader, typename FallbackLoader>
		[[nodiscard]] static ColumnMap load(
				size_t rowCount,
				const std::vector<uint8_t> &selected,
				const std::vector<std::string> &requiredProperties,
				const std::shared_ptr<storage::DataManager> &dm,
				concurrent::ThreadPool *threadPool,
				PhaseNames phases,
				RowReader &&rowReader,
				FallbackLoader &&fallbackLoader) {
			ColumnMap columns;
			if (requiredProperties.empty()) {
				return columns;
			}
			if (!selected.empty() && selected.size() != rowCount) {
				return columns;
			}

			const auto requestedProperties = deduplicateProperties(requiredProperties);
			for (const auto &key: requestedProperties) {
				columns.emplace(key, std::vector<std::optional<PropertyValue>>(rowCount, std::nullopt));
			}

			const bool traceEnabled = debug::PerfTrace::isEnabled();
			const auto extractStart = traceEnabled ? Clock::now() : Clock::time_point{};

			std::vector<int64_t> propertyEntityIds;
			std::vector<size_t> externalRows;
			std::vector<size_t> fallbackRows;
			propertyEntityIds.reserve(rowCount);
			externalRows.reserve(rowCount);
			fallbackRows.reserve(rowCount);

			for (size_t row = 0; row < rowCount; ++row) {
				if (!isSelectedRow(selected, row)) {
					continue;
				}
				const RowView view = rowReader(row);
				if (!view.active) {
					continue;
				}
				if (view.inlineProperties != nullptr) {
					assignRequestedValues(columns, requestedProperties, row, *view.inlineProperties);
				}

				if (view.propertyEntityId == 0 || view.storageType == PropertyStorageType::NONE) {
					continue;
				}
				if (view.storageType == PropertyStorageType::PROPERTY_ENTITY) {
					propertyEntityIds.push_back(view.propertyEntityId);
					externalRows.push_back(row);
				} else if (view.storageType == PropertyStorageType::BLOB_ENTITY) {
					fallbackRows.push_back(row);
				}
			}

			if (traceEnabled) {
				debug::PerfTrace::addDuration(phases.extract, elapsedNs(extractStart));
			}

			std::vector<size_t> loadedRows;
			if (dm && !propertyEntityIds.empty()) {
				const auto bulkStart = traceEnabled ? Clock::now() : Clock::time_point{};
				loadedRows = dm->bulkLoadPropertyEntityColumns(
						propertyEntityIds, externalRows, rowCount, requestedProperties, columns, threadPool);
				if (traceEnabled) {
					debug::PerfTrace::addDuration(phases.loadPropertyEntities, elapsedNs(bulkStart));
				}
			}

			const auto fillStart = traceEnabled ? Clock::now() : Clock::time_point{};
			appendRowsMissingFromBulkLoad(externalRows, std::move(loadedRows), fallbackRows);

			if (dm) {
				std::sort(fallbackRows.begin(), fallbackRows.end());
				fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
				for (const size_t row: fallbackRows) {
					assignRequestedValues(columns, requestedProperties, row, fallbackLoader(row));
				}
			}

			if (traceEnabled) {
				debug::PerfTrace::addDuration(phases.extract, elapsedNs(fillStart));
			}

			return columns;
		}

	private:
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] static uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		[[nodiscard]] static bool isSelectedRow(const std::vector<uint8_t> &selected, size_t row) {
			return selected.empty() || selected[row] != 0;
		}

		[[nodiscard]] static std::vector<std::string>
		deduplicateProperties(const std::vector<std::string> &requiredProperties) {
			std::vector<std::string> keys;
			keys.reserve(requiredProperties.size());
			for (const auto &key: requiredProperties) {
				if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
					keys.push_back(key);
				}
			}
			return keys;
		}

		static void assignRequestedValues(
				ColumnMap &columns,
				const std::vector<std::string> &requestedProperties,
				size_t row,
				const std::unordered_map<std::string, PropertyValue> &values) {
			for (const auto &key: requestedProperties) {
				auto valueIt = values.find(key);
				if (valueIt != values.end()) {
					columns[key][row] = valueIt->second;
				}
			}
		}

		static void appendRowsMissingFromBulkLoad(
				const std::vector<size_t> &externalRows,
				std::vector<size_t> loadedRows,
				std::vector<size_t> &fallbackRows) {
			std::sort(loadedRows.begin(), loadedRows.end());
			loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
			for (const size_t row: externalRows) {
				if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) {
					fallbackRows.push_back(row);
				}
			}
		}
	};

} // namespace graph::query::execution
