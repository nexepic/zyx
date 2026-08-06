/**
 * @file CatalogSegmentRetirementCoordinator.hpp
 * @brief Pre-publication lifecycle transitions for Catalog-backed segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

	class AllocatorDirectory;
	class CatalogTree;
	class MetadataRetirement;
	class MetadataWriteBatch;
	struct Superblock;

	// Stages Catalog retirement and physical-slot release transitions. Snapshot
	// admission remains with the metadata facade because it owns the writer
	// guard for the entire publication lifecycle.
	class CatalogSegmentRetirementCoordinator final {
	public:
		CatalogSegmentRetirementCoordinator(CatalogTree &catalog, AllocatorDirectory &allocator,
															LocationDirectory &locations, MetadataRetirement &retirement);

		void prepareRetirement(MetadataWriteBatch &batch, SegmentId segmentId, Generation expectedGeneration) const;
		[[nodiscard]] RetireQueueEntry validateRetiredRelease(const Superblock &current, SegmentId segmentId,
																				 Generation expectedGeneration) const;
		void prepareRetiredRelease(MetadataWriteBatch &batch, const RetireQueueEntry &retirement) const;

	private:
		CatalogTree &catalog_;
		AllocatorDirectory &allocator_;
		LocationDirectory &locations_;
		MetadataRetirement &retirement_;
	};

} // namespace graph::storage
