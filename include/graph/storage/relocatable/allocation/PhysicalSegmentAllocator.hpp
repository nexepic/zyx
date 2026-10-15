/**
 * @file PhysicalSegmentAllocator.hpp
 * @brief Durable whole-segment allocator for the experimental relocatable store.
 *
 * The allocator owns only fixed 128 KiB physical-slot initialization and the
 * data-before-metadata publication boundary. It does not interpret subtype
 * records or expose raw file offsets to persistent consumers.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	struct PhysicalSegmentAllocationRequest {
		SegmentId segment_id{};
		SegmentKind kind = SegmentKind::SGK_NONE;
		uint64_t owner_id = 0;
		Generation generation{};
		uint16_t header_bytes = RSS_ENVELOPE_BYTES;
		// Requests own their complete bounded segment image. Metadata publication
		// callbacks must never retain spans into temporary codec output.
		std::vector<std::byte> header_extension;
		std::vector<std::byte> payload;

		PhysicalSegmentAllocationRequest() = default;

		// Copies transient codec output into the request-owned image. Callers with
		// already-owned buffers can populate the public vectors and move them.
		PhysicalSegmentAllocationRequest(SegmentId segmentId, SegmentKind segmentKind, uint64_t ownerId,
									 Generation segmentGeneration, uint16_t headerBytes,
									 std::span<const std::byte> headerExtension,
									 std::span<const std::byte> segmentPayload) :
			segment_id(segmentId), kind(segmentKind), owner_id(ownerId), generation(segmentGeneration),
			header_bytes(headerBytes), header_extension(headerExtension.begin(), headerExtension.end()),
			payload(segmentPayload.begin(), segmentPayload.end()) {}
	};

	struct PhysicalSegmentAllocation {
		Superblock superblock;
		CatalogEntry entry;
	};

	struct PhysicalSegmentAllocations {
		Superblock superblock;
		std::vector<CatalogEntry> entries;
	};

	struct PhysicalSegmentContents {
		CatalogEntry entry;
		SegmentEnvelope envelope;
		std::vector<std::byte> header_extension;
		std::vector<std::byte> payload;
	};

	struct PhysicalSegmentRelocation {
		Superblock superblock;
		CatalogEntry source;
		CatalogEntry destination;
	};

	class PhysicalSegmentAllocator final {
		friend class VectorCodebookStore;

	public:
		// `io` must be the exact data-file StorageIO used by `metadata`; accepting
		// a different handle could publish a Catalog reference into the wrong file.
		PhysicalSegmentAllocator(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<PhysicalSegmentAllocations>
		allocate(std::span<const PhysicalSegmentAllocationRequest> requests, PhysicalSlotId firstSlot,
				 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		[[nodiscard]] std::optional<PhysicalSegmentAllocation>
		allocate(PhysicalSegmentAllocationRequest request, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
				 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		// Allocates immutable full-segment extents. The Catalog, free-space, and
		// Extent Directory mutations become visible through one Superblock switch,
		// after all extent bytes are durable.
		[[nodiscard]] std::optional<PhysicalSegmentAllocations>
		allocateExtentSegments(std::span<const PhysicalSegmentAllocationRequest> requests,
							   std::span<const ExtentLocation> extents, PhysicalSlotId firstSlot,
							   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
							   const MetadataPublishOptions &options = {});

		[[nodiscard]] Superblock retire(SegmentId segmentId, Generation expectedGeneration,
										LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock releaseRetired(SegmentId segmentId, Generation expectedGeneration,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &options = {});

		[[nodiscard]] std::optional<PhysicalSegmentRelocation>
		relocate(SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
				 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// The caller supplies a validated, type-specific rebuild of an immutable
		// extent header and payload. This method never raw-copies subtype bytes.
		[[nodiscard]] std::optional<PhysicalSegmentRelocation>
		relocateExtent(const ExtentLocation &expectedExtent, std::span<const std::byte> headerExtension,
					   std::span<const std::byte> payload, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
					   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock releaseRelocatedSource(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});
        [[nodiscard]] std::optional<Superblock> tryReleaseRelocatedSource(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});

		// Checked conversion used by fixed-slot I/O. The returned offset always
		// leaves room for one full physical segment.
		[[nodiscard]] static uint64_t checkedSlotOffset(uint64_t dataRegionOffset, PhysicalSlotId slot);

		// A caller must keep the metadata snapshot pin alive while it consumes the
		// returned bytes. Future relocation retirement will extend that pin to the
		// physical generation as well.
		[[nodiscard]] PhysicalSegmentContents read(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const;

	private:
		void validateRequests(std::span<const PhysicalSegmentAllocationRequest> requests) const;
		void writePreparedSegments(std::span<const CatalogEntry> entries,
								   std::span<const PhysicalSegmentAllocationRequest> requests) const;
		void copyPreparedRelocation(const CatalogEntry &source, const CatalogEntry &destination) const;
		void writeRelocatedExtentSegment(const CatalogEntry &source, const CatalogEntry &destination,
										 std::span<const std::byte> headerExtension,
										 std::span<const std::byte> payload) const;
		std::shared_ptr<StorageIO> io_;
		RelocatableMetadataStore &metadata_;
		uint64_t data_region_offset_ = 0;
	};

} // namespace graph::storage
