#pragma once

#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/data/PropertyEntitySegmentScanner.hpp"
#include "graph/utils/Serializer.hpp"

#include "PropertySerializedValueReader.hpp"
#include "PropertyScanTypes.hpp"
#include "PropertyPredicateEvaluationDetail.hpp"

namespace graph::storage {
	namespace {

		struct PropertyOwnerPredicateScanState {
			std::vector<char> readBuffer;
			std::vector<int64_t> ownerIds;
		};

		struct PropertyOwnerValueScanState {
			std::vector<char> readBuffer;
			std::vector<PropertyEntityOwnerValue> values;
		};

		struct PropertyOwnerKeyValueScanState {
			std::vector<char> readBuffer;
			std::vector<PropertyEntityOwnerKeyValue> values;
		};

		struct PropertyPredicateCountScanState {
			std::vector<char> readBuffer;
			PropertyEntityPredicateCountResult count;
		};

		struct PropertyPredicateMatchScanState {
			std::vector<char> readBuffer;
			std::vector<size_t> ignoredRows;
		};

		struct PropertyDirectVisitScanState {
			std::vector<char> readBuffer;
			size_t visited = 0;
		};

		bool canUseDirectOrderedRows(const std::vector<int64_t> &ids,
									 const std::vector<size_t> &rows,
									 size_t rowCount) {
			if (ids.empty() || rows.size() != ids.size() || rowCount == 0) {
				return false;
			}

			int64_t previousId = 0;
			size_t previousRow = 0;
			for (size_t i = 0; i < ids.size(); ++i) {
				if (ids[i] <= 0 || rows[i] >= rowCount || (i > 0 && (ids[i] <= previousId || rows[i] <= previousRow))) {
					return false;
				}
				previousId = ids[i];
				previousRow = rows[i];
			}
			return true;
		}


		template<typename EntityVisitor>
		size_t visitPropertyEntityRowsDirect(const DataManager &dm,
											 const std::vector<int64_t> &ids,
											 const std::vector<size_t> &rows,
											 EntityVisitor &&visitor) {
			if (!dm.hasPreadSupport()) {
				return 0;
			}
			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return 0; // ZYX_COV_EXCL_LINE
			}

			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work = collectPropertyEntitySegmentWork(
					std::span<const int64_t>(ids.data(), ids.size()), segIndex);
			if (work.empty()) {
				return 0;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, size_t &visited) {
				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					const int64_t id = ids[i];
					const auto slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					visited += visitor(rows[i], entityBuffer);
				}
			};

			PropertyDirectVisitScanState finalState;
			(void) detail::scanPropertyEntitySegmentWork<PropertyDirectVisitScanState>(
					dm,
					nullptr,
					"property_entity.direct_visit",
					work,
					[&](size_t, size_t, const PropertyEntitySegmentWork &w, const SegmentHeader &header,
						const char *dataBuf, PropertyDirectVisitScanState &state) {
						scanPropertyWork(w, header, dataBuf, state.visited);
					},
					[&](size_t, PropertyDirectVisitScanState &state) {
						finalState.visited += state.visited;
					});
			return finalState.visited;
		}


		template<typename Matcher>
		PropertyEntityPredicateCountResult countPropertyEntityMatches(const DataManager &dm,
																	  const std::vector<int64_t> &ids,
																	  concurrent::ThreadPool *pool,
																	  Matcher &&matchesPredicate) {
			if (ids.empty() || !dm.hasPreadSupport()) {
				return {};
			}

			bool useInputOrder = true;
			bool hasPreviousId = false;
			int64_t previousId = 0;
			for (const int64_t id: ids) {
				if (id == 0) {
					useInputOrder = false;
					continue;
				}
				if (hasPreviousId && id <= previousId) {
					useInputOrder = false;
				}
				previousId = id;
				hasPreviousId = true;
			}
			if (!hasPreviousId) {
				return {};
			}

			std::span<const int64_t> sortedIds;
			std::vector<int64_t> sortedStorage;
			std::vector<size_t> multiplicities;
			if (useInputOrder) {
				sortedIds = std::span<const int64_t>(ids.data(), ids.size());
			} else {
				sortedStorage.reserve(ids.size());
				for (const int64_t id: ids) {
					if (id != 0) {
						sortedStorage.push_back(id);
					}
				}
				std::sort(sortedStorage.begin(), sortedStorage.end());
				multiplicities.reserve(sortedStorage.size());
				size_t write = 0;
				for (size_t read = 0; read < sortedStorage.size();) {
					const int64_t id = sortedStorage[read];
					size_t next = read + 1;
					while (next < sortedStorage.size() && sortedStorage[next] == id) {
						++next;
					}
					sortedStorage[write++] = id;
					multiplicities.push_back(next - read);
					read = next;
				}
				sortedStorage.resize(write);
				sortedIds = std::span<const int64_t>(sortedStorage.data(), sortedStorage.size());
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return {}; // ZYX_COV_EXCL_LINE
			}
			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
			if (work.empty()) {
				return {};
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, PropertyEntityPredicateCountResult &targetCount) {
				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					const int64_t id = sortedIds[i];
					const auto slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					const auto matches = matchesPredicate(entityBuffer);
					if (matches.has_value() && matches.value()) {
						const size_t count = multiplicities.empty() ? size_t{1} : multiplicities[i];
						targetCount.loadedCount += count;
						targetCount.matchedCount += count;
					} else if (matches.has_value()) {
						targetCount.loadedCount += multiplicities.empty() ? size_t{1} : multiplicities[i];
					}
				}
			};

			PropertyEntityPredicateCountResult count;
			(void) detail::scanPropertyEntitySegmentWork<PropertyPredicateCountScanState>(
					dm,
					pool,
					"property_predicate_target_count.parallel",
					work,
					[&](size_t, size_t, const PropertyEntitySegmentWork &w, const SegmentHeader &header,
						const char *dataBuf, PropertyPredicateCountScanState &state) {
						scanPropertyWork(w, header, dataBuf, state.count);
					},
					[&](size_t, PropertyPredicateCountScanState &state) {
						count.loadedCount += state.count.loadedCount;
						count.matchedCount += state.count.matchedCount;
					});
			return count;
		}

		template<typename Matcher>
		PropertyEntityPredicateMatchResult matchPropertyEntityRows(const DataManager &dm,
																   const std::vector<int64_t> &ids,
																   const std::vector<size_t> &rows, size_t rowCount,
																   concurrent::ThreadPool *pool,
																   PropertyEntityPredicateMatchOptions options,
																   Matcher &&matchesPredicate) {
			PropertyEntityPredicateMatchResult result;
			if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || !dm.hasPreadSupport()) {
				return result;
			}

			std::vector<PropertyEntityRowRef> refs;
			refs.reserve(ids.size());
			std::vector<uint8_t> rowSeen(rowCount, 0);
			for (size_t i = 0; i < ids.size(); ++i) {
				if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
					rowSeen[rows[i]] = 1;
					refs.push_back({ids[i], rows[i]});
				}
			}
			if (refs.empty()) {
				return result;
			}

			auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
				if (lhs.id != rhs.id) {
					return lhs.id < rhs.id;
				}
				return lhs.row < rhs.row;
			};
			if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
				std::sort(refs.begin(), refs.end(), refLess);
			}

			std::vector<int64_t> sortedIds;
			std::vector<std::pair<size_t, size_t>> refRanges;
			sortedIds.reserve(refs.size());
			refRanges.reserve(refs.size());
			for (size_t begin = 0; begin < refs.size();) {
				size_t end = begin + 1;
				while (end < refs.size() && refs[end].id == refs[begin].id) {
					++end;
				}
				sortedIds.push_back(refs[begin].id);
				refRanges.emplace_back(begin, end);
				begin = end;
			}

			const auto segmentIndexManager = dm.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
				return result; // ZYX_COV_EXCL_LINE
			}

			const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
			const auto work =
					collectPropertyEntitySegmentWork(std::span<const int64_t>(sortedIds.data(), sortedIds.size()),
													 segIndex);
			if (work.empty()) {
				return result;
			}

			auto appendPredicateResult = [&](std::vector<size_t> &loadedRows, std::vector<size_t> *matchedRows,
											 size_t &loadedCount, size_t &matchedCount, size_t idIndex,
											 std::optional<bool> matches) {
				if (!matches.has_value()) {
					return;
				}
				const auto [refBegin, refEnd] = refRanges[idIndex];
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					++loadedCount;
					if (options.collectLoadedRows) {
						loadedRows.push_back(refs[ref].row);
					}
					if (matches.value()) {
						++matchedCount;
						if (matchedRows != nullptr) {
							matchedRows->push_back(refs[ref].row);
						}
					}
				}
			};

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
										const char *dataBuf, std::vector<size_t> &loadedRows,
										std::vector<size_t> *matchedRows, size_t &loadedCount,
										size_t &matchedCount) {
				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					const int64_t id = sortedIds[i];
					const auto slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					appendPredicateResult(loadedRows, matchedRows, loadedCount, matchedCount, i,
										  matchesPredicate(entityBuffer));
				}
			};

			std::vector<std::vector<size_t>> perWorkLoadedRows(options.collectLoadedRows ? work.size() : 0);
			std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
			std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
			std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

			(void) detail::scanPropertyEntitySegmentWork<PropertyPredicateMatchScanState>(
					dm,
					pool,
					"property_predicate_target_match.parallel",
					work,
					[&](size_t, size_t workIndex, const PropertyEntitySegmentWork &w,
						const SegmentHeader &header, const char *dataBuf, PropertyPredicateMatchScanState &state) {
						auto &loadedRows =
								options.collectLoadedRows ? perWorkLoadedRows[workIndex] : state.ignoredRows;
						std::vector<size_t> *matchedRows =
								options.collectMatchedRows ? &perWorkMatchedRows[workIndex] : nullptr;
						scanPropertyWork(w,
										 header,
										 dataBuf,
										 loadedRows,
										 matchedRows,
										 perWorkLoadedCounts[workIndex],
										 perWorkMatchedCounts[workIndex]);
					},
					[](size_t, PropertyPredicateMatchScanState &) {});

			for (size_t i = 0; i < work.size(); ++i) {
				result.loadedCount += perWorkLoadedCounts[i];
				result.matchedCount += perWorkMatchedCounts[i];
			}
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(result.loadedCount);
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(result.matchedCount);
			}
			for (size_t i = 0; i < work.size(); ++i) {
				if (options.collectLoadedRows) {
					result.loadedRows.insert(result.loadedRows.end(), perWorkLoadedRows[i].begin(),
											 perWorkLoadedRows[i].end());
				}
				if (options.collectMatchedRows) {
					result.matchedRows.insert(result.matchedRows.end(), perWorkMatchedRows[i].begin(),
											  perWorkMatchedRows[i].end());
				}
			}
			return result;
		}

		std::vector<PropertyEntityOwnerValue> collectPropertyValuesByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				const std::string &key,
				std::span<const int64_t> sortedOwnerIds,
				concurrent::ThreadPool *pool) {
			std::vector<PropertyEntityOwnerValue> values;
			if (key.empty() || !dm.hasPreadSupport()) {
				return values;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanSegmentInto = [&](const SegmentHeader &header,
									   const char *dataBuf,
									   PropertyOwnerValueScanState &state) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (readSerializedPropertyId(entityBuffer) != expectedId) {
						continue; // ZYX_COV_EXCL_LINE
					}
					auto value = readPropertyOwnerValue(entityBuffer, ownerType, key, sortedOwnerIds);
					if (value.has_value()) {
						state.values.push_back(std::move(*value));
					}
				}
			};

			(void) detail::scanAllPropertyEntitySegments<PropertyOwnerValueScanState>(
					dm,
					pool,
					"property_owner_value_scan.parallel",
					scanSegmentInto,
					[&](size_t, PropertyOwnerValueScanState &state) {
						values.insert(values.end(),
									  std::make_move_iterator(state.values.begin()),
									  std::make_move_iterator(state.values.end()));
					});
			return values;
		}

		std::vector<PropertyEntityOwnerKeyValue> collectPropertyValuesByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				const std::vector<std::string> &keys,
				std::span<const int64_t> sortedOwnerIds,
				concurrent::ThreadPool *pool) {
			std::vector<PropertyEntityOwnerKeyValue> values;
			if (keys.empty() || !dm.hasPreadSupport()) {
				return values;
			}

			std::vector<std::string> requestedKeys;
			requestedKeys.reserve(keys.size());
			for (const auto &key: keys) {
				if (key.empty() || std::find(requestedKeys.begin(), requestedKeys.end(), key) != requestedKeys.end()) {
					continue;
				}
				requestedKeys.push_back(key);
			}
			if (requestedKeys.empty()) {
				return values;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			auto scanSegmentInto = [&](const SegmentHeader &header,
									   const char *dataBuf,
									   PropertyOwnerKeyValueScanState &state) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (readSerializedPropertyId(entityBuffer) != expectedId) {
						continue; // ZYX_COV_EXCL_LINE
					}
					auto entityValues = readPropertyOwnerKeyValues(
							entityBuffer,
							ownerType,
							std::span<const std::string>(requestedKeys.data(), requestedKeys.size()),
							sortedOwnerIds);
					state.values.insert(state.values.end(),
										std::make_move_iterator(entityValues.begin()),
										std::make_move_iterator(entityValues.end()));
				}
			};

			(void) detail::scanAllPropertyEntitySegments<PropertyOwnerKeyValueScanState>(
					dm,
					pool,
					"property_owner_key_value_scan.parallel",
					scanSegmentInto,
					[&](size_t, PropertyOwnerKeyValueScanState &state) {
						values.insert(values.end(),
									  std::make_move_iterator(state.values.begin()),
									  std::make_move_iterator(state.values.end()));
					});
			return values;
		}

		template<typename Matcher>
		std::vector<int64_t> collectPropertyPredicateOwnerIdsByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				const PropertyEntityOwnerPredicateScanOptions &options,
				concurrent::ThreadPool *pool,
				Matcher &&matchesPredicate) {
			std::vector<int64_t> ownerIds;
			if (!dm.hasPreadSupport() || options.beginOwnerId > options.endOwnerId) {
				return ownerIds;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			const auto ownerTypeId = toUnderlying(ownerType);
			auto scanSegmentInto = [&](const SegmentHeader &header,
									   const char *dataBuf,
									   PropertyOwnerPredicateScanState &state) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const char *cursor = entityBuffer;
					const char *end = entityBuffer + Property::TOTAL_PROPERTY_SIZE;
					PropertyRecordHeader propertyHeader;
					if (!readPropertyRecordHeader(cursor, end, propertyHeader) ||
						!propertyHeader.active || propertyHeader.propertyId == 0 ||
						propertyHeader.entityType != ownerTypeId) {
						continue;
					}
					if (propertyHeader.entityId < options.beginOwnerId || propertyHeader.entityId > options.endOwnerId) {
						continue;
					}
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (propertyHeader.propertyId != expectedId) {
						continue;
					}
					auto matches = matchesPredicate(entityBuffer);
					if (matches.has_value() && matches.value()) {
						state.ownerIds.push_back(propertyHeader.entityId);
					}
				}
			};

			(void) detail::scanAllPropertyEntitySegments<PropertyOwnerPredicateScanState>(
					dm,
					pool,
					"property_owner_scan.parallel",
					scanSegmentInto,
					[&](size_t, PropertyOwnerPredicateScanState &state) {
						ownerIds.insert(ownerIds.end(),
										std::make_move_iterator(state.ownerIds.begin()),
										std::make_move_iterator(state.ownerIds.end()));
					});

			if (options.deduplicateOwnerIds && ownerIds.size() > 1) {
				std::sort(ownerIds.begin(), ownerIds.end());
				ownerIds.erase(std::unique(ownerIds.begin(), ownerIds.end()), ownerIds.end());
			}
			return ownerIds;
		}

		template<typename Matcher>
		PropertyEntityPredicateCountResult countPropertyEntityMatchesByOwnerType(
				const DataManager &dm,
				EntityType ownerType,
				concurrent::ThreadPool *pool,
				Matcher &&matchesPredicate) {
			PropertyEntityPredicateCountResult count;
			if (!dm.hasPreadSupport()) {
				return count;
			}

			constexpr size_t entitySize = Property::getTotalSize();
			const auto ownerTypeId = toUnderlying(ownerType);
			auto scanSegment = [&](const SegmentHeader &header,
								  const char *dataBuf,
								  PropertyPredicateCountScanState &state) {
				if (header.data_type != Property::typeId || header.used == 0) {
					return;
				}
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
					const char *cursor = entityBuffer;
					const char *end = entityBuffer + Property::TOTAL_PROPERTY_SIZE;
					PropertyRecordHeader propertyHeader;
					if (!readPropertyRecordHeader(cursor, end, propertyHeader) ||
						!propertyHeader.active || propertyHeader.propertyId == 0 ||
						propertyHeader.entityType != ownerTypeId) {
						continue;
					}
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (propertyHeader.propertyId != expectedId) {
						continue;
					}
					auto matches = matchesPredicate(entityBuffer);
					if (!matches.has_value()) {
						continue;
					}
					++state.count.loadedCount;
					if (matches.value()) {
						++state.count.matchedCount;
					}
				}
			};

			(void) detail::scanAllPropertyEntitySegments<PropertyPredicateCountScanState>(
					dm,
					pool,
					"property_predicate_count.parallel",
					scanSegment,
					[&](size_t, PropertyPredicateCountScanState &state) {
						count.loadedCount += state.count.loadedCount;
						count.matchedCount += state.count.matchedCount;
					});
			return count;
		}

	} // namespace

} // namespace graph::storage
