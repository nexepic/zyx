#include "src/storage/FileStorageFlushDetail.hpp"

#include "graph/storage/data/DataManager.hpp"

namespace graph::storage::file_storage_detail {

	FlushCacheInvalidationMode invalidateWrittenSnapshotCache(
			DataManager &dataManager,
			const FlushSnapshotView &snapshot,
			std::span<const uint64_t> touchedSegments) {
		if (touchedSegments.empty()) {
			dataManager.invalidateDirtySegments(snapshot);
			return FlushCacheInvalidationMode::FCIM_DIRTY_SNAPSHOT;
		}
		dataManager.invalidateSegments(touchedSegments);
		return FlushCacheInvalidationMode::FCIM_TOUCHED_SEGMENTS;
	}

} // namespace graph::storage::file_storage_detail
