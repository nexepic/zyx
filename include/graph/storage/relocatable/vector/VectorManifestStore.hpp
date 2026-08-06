/**
 * @file VectorManifestStore.hpp
 * @brief Atomic materialization and snapshot reads for immutable vector manifests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <memory>
#include <optional>

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	struct VectorManifestMaterializationResult {
		SharedPagePoolAllocation allocation;
		VectorManifest manifest;
	};

	class VectorManifestStore final {
	public:
		VectorManifestStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);
		VectorManifestStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
						SharedPagePoolOpenMode openMode);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<SharedPagePoolAllocation>
		allocate(SharedPagePoolAllocationRequest request, const VectorManifestPage &page, PhysicalSlotId firstSlot,
				 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// Assigns the Manifest's logical Page ID and pool Segment ID inside the
		// same transaction that makes the immutable frame and Manifest Directory
		// entry durable. Callers provide semantic fields only; page identity is
		// never guessed from offsets or process-local counters.
		[[nodiscard]] VectorManifestMaterializationResult materialize(VectorManifest manifest, PhysicalSlotId firstSlot,
																	  PhysicalSlotId lastSlot,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options = {});
		// Replaces the immutable manifest payload behind its stable logical Page ID.
		// Existing snapshots retain the prior Page Directory root and therefore keep
		// resolving the prior manifest frame until their pins are released.
		[[nodiscard]] SharedPagePoolAllocation replace(const VectorManifestPage &page, SegmentId segmentId,
													   LogSequenceNumber expectedOwnerLsn,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		// Prepares exactly one COW frame without touching a metadata root. The
		// fixed-entity compound writer publishes its Page and Manifest directories
		// with all dependent graph and Vector changes in one generation.
		[[nodiscard]] bool hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const;
		void prepareReplacement(const CatalogEntry &pool, const PageLocation &expected,
								LogSequenceNumber expectedOwnerLsn, const VectorManifestPage &page,
								LogSequenceNumber checkpointLsn, PageLocation &replacement) const;
		// Writes the replacement behind the same logical Manifest PageId into the
		// first frame of a newly reserved dedicated pool. The caller publishes the
		// Catalog, Page Directory, and Manifest Directory in one compound batch.
		void prepareNewPoolReplacement(const CatalogEntry &pool, const VectorManifestPage &page,
									   LogSequenceNumber checkpointLsn, PageLocation &replacement) const;
		// Materializes the first Manifest page for an Index without publishing a
		// directory root. First-use Vector bootstrap joins this durable frame with
		// its mapping root, raw page, and graph mutation in one compound batch.
		void prepareNewPoolInitial(const CatalogEntry &pool, const VectorManifestPage &page,
								   LogSequenceNumber checkpointLsn, PageLocation &initial) const;
		// Marks the former frame retired after a cross-pool replacement frame is
		// durable. The old Page Directory root remains snapshot-readable; recovery
		// reactivates this source if the enclosing metadata publication never completes.
		void prepareReplacementSourceRetirement(const CatalogEntry &pool, const PageLocation &expected,
												LogSequenceNumber expectedOwnerLsn,
												LogSequenceNumber checkpointLsn) const;
		// Retires this index's manifest from both logical directories in one
		// metadata generation. Existing snapshots retain access until their pins
		// drain; this does not retire the manifest's dependent vector pages.
		[[nodiscard]] Superblock retire(uint64_t indexId, LogSequenceNumber expectedOwnerLsn,
										LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] VectorManifestPage read(const MetadataSnapshotPin &snapshot, uint64_t indexId) const;
		[[nodiscard]] SharedPagePoolAllocation evacuate(uint64_t indexId, SegmentId destinationSegmentId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});
		// Resolves and validates the manifest's logical owner before relocating it.
		// Compaction plans enumerate stable Page IDs, so this avoids an unvalidated
		// reverse scan of page payloads to discover an index ID.
		[[nodiscard]] SharedPagePoolAllocation evacuatePage(PageId manifestPageId, SegmentId destinationSegmentId,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});

	private:
		[[nodiscard]] SharedPagePoolAllocation evacuateExpected(MetadataSnapshotPin &snapshot,
																const VectorManifestLocation &expected,
																SegmentId destinationSegmentId,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options);
		void validateReferencedCodebook(const VectorManifest &manifest);
		void validateReferencedVectorNodeMapping(const VectorManifest &manifest) const;
		void validateReferencedPqMapping(const VectorManifest &manifest) const;
		void validateReferencedAdjacencyMapping(const VectorManifest &manifest) const;
		RelocatableMetadataStore &metadata_;
		SharedPagePoolAllocator allocator_;
		PhysicalSegmentAllocator extents_;
	};

} // namespace graph::storage
