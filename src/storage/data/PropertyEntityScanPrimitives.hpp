#pragma once

#include "PropertyScanTypes.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "graph/core/Property.hpp"
#include "graph/storage/SegmentIndexManager.hpp"

namespace graph::storage {
	namespace {
		int64_t readSerializedPropertyId(const char *buf) {
			int64_t id = 0;
			std::memcpy(&id, buf, sizeof(int64_t));
			return id;
		}

		[[maybe_unused]] std::vector<PropertyEntitySegmentWork>
		collectPropertyEntitySegmentWork(std::span<const int64_t> sortedIds,
										 const std::vector<SegmentIndexManager::SegmentIndex> &segmentIndex) {
			std::vector<PropertyEntitySegmentWork> work;
			work.reserve(std::min(sortedIds.size(), segmentIndex.size()));
			size_t idCursor = 0;
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				const auto &entry = segmentIndex[segment];
				while (idCursor < sortedIds.size() && sortedIds[idCursor] < entry.startId) {
					++idCursor;
				}
				const size_t idBegin = idCursor;
				while (idCursor < sortedIds.size() && sortedIds[idCursor] <= entry.endId) {
					++idCursor;
				}
				if (idBegin != idCursor) {
					work.push_back({segment, idBegin, idCursor});
				}
				if (idCursor == sortedIds.size()) {
					break;
				}
			}
			return work;
		}

		[[maybe_unused]] std::vector<size_t>
		collectPropertyEntityWorkSegmentIndices(const std::vector<PropertyEntitySegmentWork> &work) {
			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(work.size());
			for (const auto &entry: work) {
				segmentIndices.push_back(entry.segmentIndex);
			}
			return segmentIndices;
		}

		bool ownerFilterContains(std::span<const int64_t> sortedOwnerIds, int64_t ownerId) {
			return sortedOwnerIds.empty() ||
				   std::binary_search(sortedOwnerIds.begin(), sortedOwnerIds.end(), ownerId);
		}

	} // namespace
} // namespace graph::storage
