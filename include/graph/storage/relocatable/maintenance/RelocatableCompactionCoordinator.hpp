/**
 * @file RelocatableCompactionCoordinator.hpp
 * @brief Bounded, snapshot-safe evacuation of relocatable relocatable page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"

namespace graph::storage {

	class SlottedPagePool;
	class EntityPageStore;
	class PhysicalSegmentAllocator;
	class BTreePageStore;
	class VectorPageStore;
	class VectorManifestStore;
	class VectorCodebookStore;
	class LargeValueStore;

	// Prefixes prevent collisions with Windows macros.
	enum class RelocatableCompactionStatus : uint8_t {
		VCS_COMPLETED = 1,
		VCS_NO_DESTINATION_CAPACITY = 2,
		VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS = 3,
	};

	struct RelocatableSlottedPoolCompactionRequest {
		SegmentId source_segment_id{};
		SegmentId destination_segment_id{};
		uint32_t page_budget = 0;
		LogSequenceNumber first_lsn{};

		constexpr bool operator==(const RelocatableSlottedPoolCompactionRequest &) const = default;
	};

	struct RelocatableSlottedPoolCompactionResult {
		RelocatableCompactionStatus status = RelocatableCompactionStatus::VCS_COMPLETED;
		uint32_t moved_pages = 0;
		bool source_empty = false;
		bool source_retired = false;
		bool source_released = false;
		LogSequenceNumber next_lsn{};

		constexpr bool operator==(const RelocatableSlottedPoolCompactionResult &) const = default;
	};

	struct RelocatableCodebookExtentCompactionRequest {
		uint64_t codebook_id = 0;
		ExtentId extent_id{};
		PhysicalSlotId first_destination_slot{};
		PhysicalSlotId last_destination_slot{};
		LogSequenceNumber first_lsn{};

		constexpr RelocatableCodebookExtentCompactionRequest() = default;
		constexpr RelocatableCodebookExtentCompactionRequest(uint64_t codebookId, PhysicalSlotId firstDestinationSlot,
													 PhysicalSlotId lastDestinationSlot, LogSequenceNumber firstLsn) :
				codebook_id(codebookId), extent_id(VectorCodebookExtentIdCodec::manifestExtentId(codebookId)),
				first_destination_slot(firstDestinationSlot),
			last_destination_slot(lastDestinationSlot), first_lsn(firstLsn) {}
		constexpr RelocatableCodebookExtentCompactionRequest(uint64_t codebookId, ExtentId extentId,
													 PhysicalSlotId firstDestinationSlot,
													 PhysicalSlotId lastDestinationSlot, LogSequenceNumber firstLsn) :
			codebook_id(codebookId), extent_id(extentId), first_destination_slot(firstDestinationSlot),
			last_destination_slot(lastDestinationSlot), first_lsn(firstLsn) {}

		constexpr bool operator==(const RelocatableCodebookExtentCompactionRequest &) const = default;
	};

	struct RelocatableExtentCompactionResult {
		RelocatableCompactionStatus status = RelocatableCompactionStatus::VCS_COMPLETED;
		bool source_relocated = false;
		bool source_released = false;
		LogSequenceNumber next_lsn{};

		constexpr bool operator==(const RelocatableExtentCompactionResult &) const = default;
	};

	// Coordinates only page formats whose stable references are PageId/slot
	// pairs and whose page payload is safely rebound by SlottedPagePool. Typed
	// formats with an additional generation-bearing page header use their own
	// evacuation coordinators rather than a raw-byte move.
	class RelocatableCompactionCoordinator final {
	public:
		RelocatableCompactionCoordinator(SlottedPagePool &pages, RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, EntityPageStore &entities,
												   RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, BTreePageStore &btreePages, RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorPageStore &vectorPages, RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorManifestStore &vectorManifests,
								   RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, LargeValueStore &largeValues,
								   RelocatableMetadataStore &metadata);
		RelocatableCompactionCoordinator(SlottedPagePool &pages, VectorCodebookStore &codebooks,
								   PhysicalSegmentAllocator &physical, RelocatableMetadataStore &metadata);

		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactSlottedPool(const RelocatableSlottedPoolCompactionRequest &request,
						   const MetadataPublishOptions &options = {});
		// Entity pools are page-addressed and may contain Node, Edge, and typed
		// auxiliary pages. Each frame is relocated through EntityPageStore so its
		// Page Directory and Entity Map invariants remain authoritative.
		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactEntityPool(const RelocatableSlottedPoolCompactionRequest &request,
						  const MetadataPublishOptions &options = {});

		// Rebuilds B+Tree pages with the destination pool generation and atomically
		// rebinds a root's Index Root Directory entry when required.
		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactBTreePool(const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &options = {});

		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactVectorPool(const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &options = {});

		// Rebuilds immutable vector manifests and atomically rebinds both Page and
		// Vector Manifest directories while preserving each logical Manifest Page ID.
		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactVectorManifestPool(const RelocatableSlottedPoolCompactionRequest &request,
								  const MetadataPublishOptions &options = {});
		// Dispatches LargeValue data pages through the Page Directory-only path and
		// manifest pages through the typed Manifest Directory rebind path.
		[[nodiscard]] RelocatableSlottedPoolCompactionResult
		compactLargeValuePool(const RelocatableSlottedPoolCompactionRequest &request,
								  const MetadataPublishOptions &options = {});

		// Returns false when a pinned snapshot can still resolve the retired pool.
		// A later scheduler run can retry with the same segment generation.
		[[nodiscard]] bool releaseRetiredSlottedPool(SegmentId segmentId, Generation generation,
															 LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options = {});
		[[nodiscard]] bool releaseRetiredEntityPool(SegmentId segmentId, Generation generation,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});

		[[nodiscard]] RelocatableExtentCompactionResult
		compactCodebookExtent(const RelocatableCodebookExtentCompactionRequest &request,
							  const MetadataPublishOptions &options = {});
		[[nodiscard]] bool releaseRelocatedExtentSource(SegmentId segmentId, Generation generation,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});

	private:
		SlottedPagePool &pages_;
		RelocatableMetadataStore &metadata_;
		EntityPageStore *entity_pages_ = nullptr;
		PhysicalSegmentAllocator *physical_ = nullptr;
		BTreePageStore *btree_pages_ = nullptr;
		VectorPageStore *vector_pages_ = nullptr;
		VectorManifestStore *vector_manifests_ = nullptr;
		LargeValueStore *large_values_ = nullptr;
		VectorCodebookStore *codebooks_ = nullptr;
	};

} // namespace graph::storage
