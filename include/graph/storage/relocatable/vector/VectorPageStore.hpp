/**
 * @file VectorPageStore.hpp
 * @brief Durable raw/PQ VectorPage materialization through shared page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"

namespace graph::storage {

	struct RawVectorMaterializationRecord {
		uint64_t vector_id = 0;
		std::vector<std::byte> bytes;

		bool operator==(const RawVectorMaterializationRecord &) const = default;
	};

	struct VectorRawMaterializationOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_records = 0;
		size_t maximum_pages = 0;
	};

	struct VectorRawMaterializationPage {
		PageId page_id{};
		uint32_t record_count = 0;

		constexpr bool operator==(const VectorRawMaterializationPage &) const = default;
	};

	struct VectorRawMaterializationResult {
		Superblock superblock;
		uint64_t record_count = 0;
		std::vector<VectorRawMaterializationPage> pages;

		bool operator==(const VectorRawMaterializationResult &) const = default;
	};

	struct PqVectorMaterializationRecord {
		uint64_t vector_id = 0;
		// One byte per codebook subspace. VectorPageStore prepends vector_id in
		// the durable fixed record so a page-local scan can verify a mapping hit.
		std::vector<uint8_t> codes;

		bool operator==(const PqVectorMaterializationRecord &) const = default;
	};

	struct VectorPqMaterializationOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		Generation codebook_generation{};
		size_t maximum_records = 0;
		size_t maximum_pages = 0;
	};

	struct VectorPqPageMapping {
		uint64_t vector_id = 0;
		PageId page_id{};

		constexpr bool operator==(const VectorPqPageMapping &) const = default;
	};

	struct VectorPqMaterializationPage {
		PageId page_id{};
		uint32_t record_count = 0;

		constexpr bool operator==(const VectorPqMaterializationPage &) const = default;
	};

	struct VectorPqMaterializationResult {
		Superblock superblock;
		uint64_t record_count = 0;
		std::vector<VectorPqMaterializationPage> pages;
		// This in-memory output is the canonical, sorted input for the later
		// immutable VectorId -> PQ PageId mapping-tree publication.
		std::vector<VectorPqPageMapping> mappings;

		bool operator==(const VectorPqMaterializationResult &) const = default;
	};

	// A current PQ page observation usable by a generation-fenced derived-data
	// reclaimer. A raw Vector Location Directory reference is checked separately
	// because legacy direct PQ pages share the physical pool kind.
	struct DerivedVectorPqPage {
		PageLocation location;
		uint64_t index_id = 0;
		LogSequenceNumber owner_lsn{};

		constexpr bool operator==(const DerivedVectorPqPage &) const = default;
	};

	// A raw page is reclaimable only after its entire logical Vector index has
	// been removed from the schema view. The owner LSN fences the page against a
	// stale maintenance scan before its Vector Location entries disappear.
	struct RetiredRawVectorPage {
		PageLocation location;
		uint64_t index_id = 0U;
		LogSequenceNumber owner_lsn{};

		constexpr bool operator==(const RetiredRawVectorPage &) const = default;
	};

	class RelocatableMetadataStore;

	class VectorPageStore final {
	public:
		VectorPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);
		VectorPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
						SharedPagePoolOpenMode openMode);
		// Calculates the maximum fixed-record occupancy that leaves room for the
		// bitmap and durable slot-generation table in one Vector page.
		[[nodiscard]] static uint16_t rawSlotCapacity(uint32_t recordBytes);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<SharedPagePoolAllocation>
		allocate(SharedPagePoolAllocationRequest request, const VectorPage &page, PhysicalSlotId firstSlot,
				 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		// Writes one canonical raw-vector generation with data-before-metadata
		// ordering. Every record includes its immutable Vector ID prefix; IDs are
		// published atomically with their Page/slot locations.
		[[nodiscard]] VectorRawMaterializationResult
		materializeRaw(uint64_t indexId, std::span<const RawVectorMaterializationRecord> records,
					   const VectorRawMaterializationOptions &options, LogSequenceNumber checkpointLsn,
					   const MetadataPublishOptions &publishOptions = {});
		// Stages derived PQ frames without writing a raw Vector Location entry.
		// The caller must publish `mappings` through the dedicated PQ mapping tree
		// and immutable Manifest before treating these pages as query-visible.
		[[nodiscard]] VectorPqMaterializationResult
		materializePq(uint64_t indexId, std::span<const PqVectorMaterializationRecord> records,
					  const VectorPqMaterializationOptions &options, LogSequenceNumber checkpointLsn,
					  const MetadataPublishOptions &publishOptions = {});
		// Materializes derived PQ frames only after an enclosing metadata batch has
		// assigned every pool, PageId, and physical slot. No directory root is
		// touched here: the caller publishes the Page Directory, mapping tree, and
		// Vector Manifest together through its one COW generation.
		[[nodiscard]] VectorPqMaterializationResult
		preparePqNewPoolPages(uint64_t indexId, std::span<const PqVectorMaterializationRecord> records,
							  const VectorPqMaterializationOptions &options,
							  std::span<VectorPqMaterializationPoolBatch> batches,
							  LogSequenceNumber checkpointLsn,
							  const CancellationCheckpoint *cancellation = nullptr) const;
		// Marks previously visible derived PQ frames retired without changing a
		// directory root. An enclosing rebuild batch must erase their PageIds and
		// publish its replacement mapping and Manifest in that same generation.
		void prepareDerivedPqRetirements(std::span<const ExistingPoolPageRetirementBatch> batches,
										LogSequenceNumber checkpointLsn,
										const CancellationCheckpoint *cancellation = nullptr) const;
		[[nodiscard]] SharedPagePoolAllocation append(SharedPagePoolAppendRequest request, const VectorPage &page,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		[[nodiscard]] SharedPagePoolAllocation replace(const VectorPage &page, SegmentId segmentId,
													   LogSequenceNumber expectedOwnerLsn,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		// These two narrow primitives are intentionally preparation-only: callers
		// run them from the fixed-entity compound metadata callback, then publish
		// the matching Page Directory entry with the graph and mapping roots.
		[[nodiscard]] bool hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const;
		void prepareRawReplacement(const CatalogEntry &pool, const PageLocation &expected,
								   LogSequenceNumber expectedOwnerLsn, const VectorPage &page,
								   LogSequenceNumber checkpointLsn, PageLocation &replacement) const;
		// Prepares a fresh raw page in an already-published raw pool. The caller
		// obtains the logical Page ID from its enclosing metadata batch and must
		// publish the returned Page Directory location in that same batch.
		void prepareRawAppend(const CatalogEntry &pool, const VectorPage &page, LogSequenceNumber checkpointLsn,
							  PageLocation &appended) const;
		// Materializes the first raw page in a newly reserved pool. Its Catalog and
		// Page Directory entries remain unpublished until the enclosing compound
		// metadata batch switches every dependent root together.
		void prepareRawNewPool(const CatalogEntry &pool, const VectorPage &page, LogSequenceNumber checkpointLsn,
							   PageLocation &initial) const;
		// Materializes several initial Raw pages in one fresh pool. Callers assign
		// stable page IDs under the enclosing metadata writer; none are reachable
		// until that same generation publishes every Page Directory entry.
		void prepareRawNewPoolPages(const CatalogEntry &pool, std::span<const VectorPage> pages,
									std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) const;
		// Point lookup is the default. Scan-oriented callers must explicitly opt
		// into sequential admission so they do not retain an unbounded Vector page set.
		[[nodiscard]] VectorPage read(const MetadataSnapshotPin &snapshot, PageId pageId,
									  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		[[nodiscard]] std::optional<DerivedVectorPqPage> inspectDerivedPqPage(const MetadataSnapshotPin &snapshot,
																			  PageId pageId) const;
		[[nodiscard]] Superblock retireDerivedPqPages(Generation expectedMetadataGeneration,
													  std::span<const DerivedVectorPqPage> pages,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &publishOptions = {});
		// `erasedVectorIds` may be empty when a Raw page contains only immutable
		// records whose Node mappings were removed by earlier transactions.
		[[nodiscard]] Superblock retireRawPages(Generation expectedMetadataGeneration,
												std::span<const RetiredRawVectorPage> pages,
												std::span<const uint64_t> erasedVectorIds,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &publishOptions = {});
		// A retired Raw pool is released only after the metadata snapshot registry
		// proves that no old Page Directory generation can resolve its frames.
		[[nodiscard]] Superblock retireEmptyRawPool(SegmentId segmentId, Generation expectedGeneration,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] Superblock releaseRetiredRawPool(SegmentId segmentId, Generation expectedGeneration,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &publishOptions = {});
		void reclaimRetiredFrames(SegmentId segmentId) const;
		// Relocates one raw/PQ frame while retaining stable Page ID, slot ordinal,
		// and slot generation references. Raw and PQ pools remain disjoint.
		[[nodiscard]] SharedPagePoolAllocation evacuate(const VectorPage &page, SegmentId destinationSegmentId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});

	private:
		[[nodiscard]] static uint32_t pageFreeBytes(const VectorPage &page);
		[[nodiscard]] static SegmentKind poolKind(VectorStorageKind kind);
		SharedPagePoolAllocator allocator_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
