/**
 * @file VectorCodebookStore.hpp
 * @brief Immutable inline and multi-extent Vector Codebook storage for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorCodebook.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"

namespace graph::storage {

	struct VectorCodebookDataExtent {
		ExtentId extent_id{};
		SegmentId segment_id{};

		constexpr bool operator==(const VectorCodebookDataExtent &) const = default;
	};

	struct VectorCodebookWriteRequest {
		uint64_t codebook_id = 0;
		SegmentId manifest_segment_id{};
		std::vector<VectorCodebookDataExtent> data_extents;
		Generation generation{};
		VectorCodebookElementType element_type = VectorCodebookElementType::VCET_FLOAT32;
		uint32_t dimensions = 0;
		uint32_t subspace_count = 0;
		uint32_t centroid_count = 0;
	};

	struct VectorCodebook {
		VectorCodebookHeader header;
		std::vector<std::byte> payload;

		bool operator==(const VectorCodebook &) const = default;
	};

	// A Codebook candidate is immutable logical content plus the exact extent
	// shape required to make it durable. It has no physical-slot assignment and
	// cannot publish metadata by itself, so an enclosing COW transaction can
	// reserve Catalog entries before the typed writer materializes its bytes.
	struct VectorCodebookPreparedExtent {
		SegmentId segment_id{};
		ExtentLocation location;
		VectorCodebookExtentHeaderCodec::Bytes header;
		std::vector<std::byte> payload;
	};

	class VectorCodebookPreparedWrite final {
	public:
		[[nodiscard]] uint64_t codebookId() const { return codebook_id_; }
		[[nodiscard]] Generation generation() const { return generation_; }
		[[nodiscard]] const VectorCodebookManifest &manifest() const { return manifest_; }
		[[nodiscard]] std::span<const VectorCodebookPreparedExtent> extents() const { return extents_; }
		[[nodiscard]] std::span<const ExtentLocation> extentLocations() const { return locations_; }
		[[nodiscard]] std::vector<CatalogSlotReservationRequest> catalogReservations() const;

	private:
		friend class VectorCodebookStore;

		uint64_t codebook_id_ = 0U;
		Generation generation_{};
		VectorCodebookManifest manifest_;
		std::vector<VectorCodebookPreparedExtent> extents_;
		// Kept in ExtentId order for the metadata directory. `extents_` remains in
		// SegmentId order for physical Catalog reservation and write validation.
		std::vector<ExtentLocation> locations_;
	};

	class VectorCodebookStore final {
	public:
		VectorCodebookStore(PhysicalSegmentAllocator &segments, RelocatableMetadataStore &metadata,
							size_t maximumCacheBytes = 0U, size_t maximumCacheEntries = 0U);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata && segments_.usesMetadataStore(metadata);
		}

		// Creates one immutable Codebook object. Small payloads remain inline in
		// the manifest extent; larger payloads publish all data extents and the
		// stable Codebook-ID manifest extent in one metadata generation.
		[[nodiscard]] Superblock writeImmutable(const VectorCodebookWriteRequest &request,
												std::span<const std::byte> payload, PhysicalSlotId firstSlot,
												PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &options = {});
		// Prepares one immutable Codebook without assigning physical slots or
		// publishing metadata. The returned candidate is consumed only after an
		// enclosing transaction has reserved matching Catalog and Extent entries.
		[[nodiscard]] static VectorCodebookPreparedWrite prepareImmutable(const VectorCodebookWriteRequest &request,
																		  std::span<const std::byte> payload);
		// Writes a prepared candidate to metadata-reserved Catalog entries. This
		// method deliberately does not update Catalog or Extent Directory roots;
		// callers must use it only from their matching data-before-metadata
		// preparer. Identity, generation, LSN, extent shape, and typed payload
		// checks complete before any physical frame is written.
		void writePreparedImmutable(const VectorCodebookPreparedWrite &candidate,
									std::span<const CatalogEntry> entries) const;
		[[nodiscard]] VectorCodebookManifest manifest(const MetadataSnapshotPin &snapshot, uint64_t codebookId) const;
		[[nodiscard]] VectorCodebook read(const MetadataSnapshotPin &snapshot, uint64_t codebookId) const;
		[[nodiscard]] VectorCodebook readBounded(const MetadataSnapshotPin &snapshot, uint64_t codebookId,
												 uint64_t maximumPayloadBytes, size_t maximumDataExtents) const;
		[[nodiscard]] std::shared_ptr<const VectorCodebook> readSharedBounded(const MetadataSnapshotPin &snapshot,
																			  uint64_t codebookId,
																			  uint64_t maximumPayloadBytes,
																			  size_t maximumDataExtents) const;
		void validate(const MetadataSnapshotPin &snapshot, uint64_t codebookId, uint64_t maximumPayloadBytes,
					  size_t maximumDataExtents) const;
		// Relocates one physical extent while retaining the immutable logical
		// Codebook generation. Manifest references therefore never change merely
		// because either the manifest or a data extent moves.
		[[nodiscard]] std::optional<PhysicalSegmentRelocation>
		relocateExtent(uint64_t codebookId, ExtentId extentId, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
					   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] static size_t requiredDataExtentCount(size_t payloadBytes) {
			return VectorCodebookManifestCodec::requiredDataExtentCount(payloadBytes);
		}

	private:
		struct CacheKey {
			uint64_t codebook_id = 0U;
			uint64_t generation = 0U;

			constexpr auto operator<=>(const CacheKey &) const = default;
		};

		struct CacheEntry {
			std::shared_ptr<const VectorCodebook> codebook;
			std::list<CacheKey>::iterator recency;
		};

		[[nodiscard]] std::vector<std::byte> readDataExtent(const MetadataSnapshotPin &snapshot,
															const VectorCodebookManifest &manifest,
															const VectorCodebookManifestEntry &entry,
															size_t ordinal) const;
		PhysicalSegmentAllocator &segments_;
		RelocatableMetadataStore &metadata_;
		size_t maximum_cache_bytes_ = 0U;
		size_t maximum_cache_entries_ = 0U;
		mutable size_t cached_payload_bytes_ = 0U;
		mutable std::mutex cache_mutex_;
		mutable std::list<CacheKey> cache_recency_;
		mutable std::map<CacheKey, CacheEntry> cache_;
	};

} // namespace graph::storage
