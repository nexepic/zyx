/**
 * @file CatalogRetireQueueReclaimer.hpp
 * @brief Snapshot-safe batch reclamation for Catalog-backed physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <span>
#include <vector>

#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

	class AllocatorDirectory;
	class CatalogTree;
	class MetadataRetirement;
	class MetadataWriteBatch;
	class MetadataWriteGuard;

	// Collects only the retire-queue entries that no active snapshot can still
	// resolve, then stages their Catalog, queue, and allocator transitions in one
	// publication. Locking and the final Superblock publication stay with the facade.
	class CatalogRetireQueueReclaimer final {
	public:
		CatalogRetireQueueReclaimer(CatalogTree &catalog, AllocatorDirectory &allocator, LocationDirectory &locations,
									MetadataRetirement &retirement);

		[[nodiscard]] std::vector<RetireQueueEntry> collectReclaimable(const Superblock &current,
																	   const MetadataWriteGuard &writer) const;
		void prepareReclamation(MetadataWriteBatch &batch, std::span<const RetireQueueEntry> entries) const;

	private:
		void validateEntry(const Superblock &current, const RetireQueueEntry &entry) const;

		CatalogTree &catalog_;
		AllocatorDirectory &allocator_;
		LocationDirectory &locations_;
		MetadataRetirement &retirement_;
	};

} // namespace graph::storage
