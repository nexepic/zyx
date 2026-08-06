/**
 * @file MetadataRetirement.hpp
 * @brief Snapshot-safe validation for relocatable physical-slot retirement.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <span>

#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"

namespace graph::storage {

	class CatalogTree;
	class MetadataWriteBatch;

	// Validates durable retirement queue transitions and determines whether an
	// active snapshot can still resolve a retiring physical slot. Publication and
	// post-publication free-space release remain owned by the metadata facade.
	class MetadataRetirement final {
	public:
		MetadataRetirement(CatalogTree &catalog, LocationDirectory &locations,
						  MetadataSnapshotRegistry &snapshots);

		void prepareQueueInsertions(MetadataWriteBatch &batch, std::span<const RetireQueueEntry> entries) const;
		[[nodiscard]] bool isVisibleToActiveSnapshot(const MetadataWriteGuard &writer,
															const RetireQueueEntry &entry) const;

	private:
		CatalogTree &catalog_;
		LocationDirectory &locations_;
		MetadataSnapshotRegistry &snapshots_;
	};

} // namespace graph::storage
