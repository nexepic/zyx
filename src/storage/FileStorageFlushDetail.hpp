#pragma once

#include <cstdint>
#include <span>

namespace graph::storage {
	class DataManager;
	struct FlushSnapshotView;

	namespace file_storage_detail {

		enum class FlushCacheInvalidationMode {
			FCIM_DIRTY_SNAPSHOT,
			FCIM_TOUCHED_SEGMENTS,
		};

		FlushCacheInvalidationMode invalidateWrittenSnapshotCache(
				DataManager &dataManager,
				const FlushSnapshotView &snapshot,
				std::span<const uint64_t> touchedSegments);

	} // namespace file_storage_detail
} // namespace graph::storage
