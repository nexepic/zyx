/**
 * @file CatalogSegmentRelocationCoordinator.hpp
 * @brief Pre-publication relocation coordination for Catalog-backed segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <optional>

#include "graph/storage/relocatable/metadata/CatalogSlotRelocation.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

	class AllocatorDirectory;
	class CatalogTree;
	class MetadataRetirement;
	class MetadataWriteBatch;

	// The coordinator returns the durable data-copy plan. The facade attaches the
	// published Superblock only after the candidate roots are atomically visible.
	struct CatalogSlotRelocationPlan {
		CatalogEntry source;
		CatalogEntry destination;
	};

	// Coordinates source validation, target-slot reservation, Catalog switch,
	// relocation retirement, and optional immutable Extent rebinding. The caller
	// retains writer locking, snapshot visibility admission, and root publication.
	class CatalogSegmentRelocationCoordinator final {
	public:
		CatalogSegmentRelocationCoordinator(CatalogTree &catalog, AllocatorDirectory &allocator,
											LocationDirectory &locations, MetadataRetirement &retirement);

		[[nodiscard]] std::optional<CatalogSlotRelocationPlan>
		prepareSegmentRelocation(MetadataWriteBatch &batch, SegmentId segmentId, Generation expectedGeneration,
								 PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
								 const CatalogSlotRelocationPreparer &preparer) const;

		[[nodiscard]] std::optional<CatalogSlotRelocationPlan>
		prepareExtentRelocation(MetadataWriteBatch &batch, const ExtentLocation &expectedExtent,
								PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
								const CatalogSlotRelocationPreparer &preparer) const;

		[[nodiscard]] RetireQueueEntry validateRelocatedSourceRelease(const Superblock &current, SegmentId segmentId,
																	  Generation expectedGeneration) const;
		void prepareRelocatedSourceRelease(MetadataWriteBatch &batch, const RetireQueueEntry &retirement) const;

	private:
		[[nodiscard]] std::optional<RetireQueueEntry> findPendingSource(const Superblock &current,
																		SegmentId segmentId) const;

		CatalogTree &catalog_;
		AllocatorDirectory &allocator_;
		LocationDirectory &locations_;
		MetadataRetirement &retirement_;
	};

} // namespace graph::storage
