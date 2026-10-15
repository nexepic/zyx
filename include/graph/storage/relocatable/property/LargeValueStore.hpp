/**
 * @file LargeValueStore.hpp
 * @brief Atomic immutable persistence and snapshot reads for generic LargeValue records.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/utils/OwnedVector.hpp"

namespace graph::storage {
	class PropertyCollectionRootStore;
	class SlottedPagePool;

	// A reserved content-index mutation remains separate from the enclosing
	// graph publication. The caller composes it with the Property references and
	// invokes the supplied preparer only after metadata assigns page identities.
	struct LargeValueContentIndexPublicationPlan {
		NativeGraphIndexTreeMutation mutation;
		BTreeStructuralMutationPreparer preparer;
	};

	// Physical ranges remain a caller policy, while the store retains logical
	// identity and publishes all selected pools under one metadata generation.
	struct LargeValueWriteOptions {
		PhysicalSlotId first_data_slot{};
		PhysicalSlotId last_data_slot{};
		PhysicalSlotId first_manifest_slot{};
		PhysicalSlotId last_manifest_slot{};
		LargeValueKind kind = LargeValueKind::LVK_BYTES;
		std::string media_type;
		LargeValueCompressionOptions compression{};
		LargeValueExtentClass extent_class = LargeValueExtentClass::LVEC_32K;
		// This cap bounds collision/corruption work per lookup. It is deliberately
		// independent from B+Tree page capacity and must be supplied by callers
		// that need a tighter tenant-specific maintenance budget.
		size_t maximum_dedup_candidates = RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES;
	};

	struct LargeValueWriteResult {
		Superblock superblock;
		LargeValueRef reference;
		LargeValueManifest manifest;
		bool content_deduplicated = false;
		size_t dedup_candidates_scanned = 0U;
	};

	// Exposes the bounded amount of content-index work so callers can meter and
	// tune deduplication without inferring it from storage-side effects.
	struct LargeValueDedupLookupResult {
		std::optional<LargeValueRef> reference;
		size_t candidates_scanned = 0U;
	};

	// The streamed graph coordinator assigns only logical PageIds. This request
	// lets LargeValueStore encode one immutable value with a page-local chunk
	// buffer and a bounded manifest frontier, without constructing the generic
	// vector-backed transaction materialization result.
	struct LargeValueStreamedMaterializationRequest {
		LargeValueRef reference;
		std::span<const std::byte> logical_bytes;
		LargeValueCompressionOptions compression{};
		LargeValueExtentClass extent_class = LargeValueExtentClass::LVEC_32K;
		PageId first_data_page_id{};
		size_t data_page_count = 0U;
		PageId first_manifest_page_id{};
		size_t manifest_page_count = 0U;
	};

	using LargeValueStreamedOwnerResolver = std::function<PageOwnerHeader(PageId, PageOwnerKind)>;
	using LargeValueStreamedFrameVisitor =
			std::function<void(const PageOwnerHeader &, std::vector<std::byte>, PageOwnerKind)>;

	class LargeValueStore final {
	public:
		LargeValueStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
						SharedPagePoolOpenMode openMode = SharedPagePoolOpenMode::SPPOM_RECONCILE);
		~LargeValueStore();
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}
		// The content index is a reserved internal B+Tree. It is exposed only to
		// typed maintenance and graph-publication paths that can include its root
		// change in the same metadata batch as a LargeValue reference.
		[[nodiscard]] BTreePageStore &contentIndex() const;
		// Looks up a content-equivalent immutable value through the persistent
		// full-identity index and re-authenticates the candidate bytes. A caller
		// may reuse a returned reference only in the same pinned snapshot.
		[[nodiscard]] std::optional<LargeValueRef> findContentDuplicate(const MetadataSnapshotPin &snapshot,
																		std::span<const std::byte> bytes,
																		LargeValueKind kind, std::string_view mediaType,
																		size_t maximumCandidates) const;
		[[nodiscard]] LargeValueDedupLookupResult
		findContentDuplicateWithStats(const MetadataSnapshotPin &snapshot, std::span<const std::byte> bytes,
									  LargeValueKind kind, std::string_view mediaType, size_t maximumCandidates) const;
		[[nodiscard]] std::vector<std::byte> contentIndexKey(const LargeValueRef &reference) const;
		// Plans the reserved content-identity index update for new immutable values.
		// This does not publish anything; callers must place the returned mutation
		// in the same NativeGraphDerivedStateMutation as the Property references.
		[[nodiscard]] LargeValueContentIndexPublicationPlan planNativeGraphContentIndexMutation(
				const MetadataSnapshotPin &snapshot, std::span<const NativeGraphLargeValueMutation> values,
				PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, bool forceFreshRebuild = false) const;
		// Computes the immutable logical identity that a later native graph
		// transaction will materialize. This makes codec fallback deterministic
		// before the small Property reference becomes reachable.
		[[nodiscard]] LargeValueRef
		planReference(LargeValueId valueId, Generation generation, std::span<const std::byte> bytes,
					  LargeValueKind kind, std::string mediaType, const LargeValueCompressionOptions &compression,
					  LargeValueExtentClass extentClass = LargeValueExtentClass::LVEC_32K) const;
		[[nodiscard]] size_t
		plannedDataPageCount(std::span<const std::byte> bytes, const LargeValueCompressionOptions &compression,
							 LargeValueExtentClass extentClass = LargeValueExtentClass::LVEC_32K) const;
		[[nodiscard]] size_t
		plannedManifestPageCount(std::span<const std::byte> bytes, const LargeValueCompressionOptions &compression,
								 LargeValueExtentClass extentClass = LargeValueExtentClass::LVEC_32K) const;
		// Emits data then Manifest frames for one immutable value. The caller must
		// synchronously place each frame into an already reserved shared pool and
		// append its directory route before returning from the visitor.
		void materializeStreamedValue(const LargeValueStreamedMaterializationRequest &request,
									 LogSequenceNumber checkpointLsn,
									 const LargeValueStreamedOwnerResolver &resolveOwner,
									 const LargeValueStreamedFrameVisitor &visit) const;

		// Writes the immutable data frames and manifest before publishing their
		// Catalog, Page Directory, and LargeValue directory roots together.
		[[nodiscard]] LargeValueWriteResult write(std::span<const std::byte> bytes,
												  const LargeValueWriteOptions &options,
												  LogSequenceNumber checkpointLsn,
												  const MetadataPublishOptions &publishOptions = {});
		// Maintenance reads an immutable Manifest by logical identity before it
		// decides whether every associated frame is still reachable.
		[[nodiscard]] LargeValueManifest manifest(const MetadataSnapshotPin &snapshot, LargeValueId valueId,
				const std::function<void(size_t)> &bufferAdmission = {}) const;
		[[nodiscard]] LargeValueManifest manifest(const MetadataSnapshotPin &snapshot,
												  const LargeValueRef &reference,
				const std::function<void(size_t)> &bufferAdmission = {}) const;
		// Resolves and validates every node reachable from a directory-routed root.
		// Maintenance uses this rather than assuming all chunks reside in the root.
		[[nodiscard]] std::vector<LargeValueManifest> manifestTree(const MetadataSnapshotPin &snapshot,
																   const LargeValueManifest &root) const;
		[[nodiscard]] std::vector<std::byte> read(const MetadataSnapshotPin &snapshot,
												  const LargeValueRef &reference, const std::function<void(size_t)> &bufferAdmission = {}) const;
		// Reads only chunk pages that overlap [offset, offset + length). A zero
		// length range is valid and still authenticates the manifest reference.
		[[nodiscard]] std::vector<std::byte> readRange(const MetadataSnapshotPin &snapshot,
													   const LargeValueRef &reference, uint64_t offset,
													   uint64_t length) const;
		// Reads through an already authenticated manifest. Property column readers
		// use this to avoid reopening the same immutable manifest for every token.
		[[nodiscard]] std::vector<std::byte> readRangeFromManifest(const MetadataSnapshotPin &snapshot,
																   const LargeValueManifest &manifest, uint64_t offset,
																   uint64_t length, const std::function<void(size_t)> &bufferAdmission = {}) const;
		// Caller-local reuse for adjacent ranges. Retains at most one format-sized
		// authenticated raw chunk, never pins or a second persistent truth.
		class RangeReadCache final {
		public:
			RangeReadCache() = default;
			RangeReadCache(const RangeReadCache &) = delete;
			RangeReadCache &operator=(const RangeReadCache &) = delete;
			RangeReadCache(RangeReadCache &&) noexcept = default;
			RangeReadCache &operator=(RangeReadCache &&) noexcept = default;
			[[nodiscard]] size_t retainedBytes() const { return raw_.capacity(); }
			void clear() { valid_ = false; utils::OwnedVector<std::byte>(raw_.get_allocator()).swap(raw_); }
		private:
			friend class LargeValueStore;
			bool valid_ = false;
			const LargeValueStore *store_ = nullptr;
			std::weak_ptr<StorageIO> io_;
			Superblock snapshot_{};
			uint64_t mutation_epoch_ = 0U;
			LargeValueId value_id_{};
			Generation generation_{};
			LargeValueChunk chunk_{};
			utils::OwnedVector<std::byte> raw_;
		};
		[[nodiscard]] std::vector<std::byte> readRangeFromManifestCached(const MetadataSnapshotPin &snapshot,
			const LargeValueManifest &manifest, uint64_t offset, uint64_t length, RangeReadCache &cache,
			const std::function<void(size_t)> &bufferAdmission = {}) const;
		// Removes complete unreachable values: all data pages and the one manifest
		// page are marked retired, their Page Directory entries disappear, and the
		// matching LargeValue Manifest Directory entries are erased atomically.
		[[nodiscard]] Superblock retireUnreachablePages(Generation expectedMetadataGeneration,
														std::span<const LargeValueId> erasedValueIds,
														std::span<const PageRetirement> retirements,
														PhysicalSlotId firstIndexSlot, PhysicalSlotId lastIndexSlot,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &publishOptions = {});
		// Physical frame reuse is delayed until all snapshots that can resolve a
		// retired Page Directory root have drained. Pool retirement then releases
		// the whole single-file slot through the Catalog/Retire Queue protocol.
		void reclaimRetiredFrames(SegmentId segmentId) const;
		[[nodiscard]] Superblock retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] Superblock releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &publishOptions = {});
		// Relocates the immutable manifest for one logical value. Its directory
		// route and Page Directory location switch in the same COW publication;
		// the LargeValueRef's logical generation remains unchanged.
		[[nodiscard]] SharedPagePoolAllocation evacuate(LargeValueId valueId, SegmentId destinationSegmentId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &publishOptions = {});
		// Relocates one verified LargeValue data or manifest page. A manifest page
		// takes the typed directory-rebind path; data pages are reachable only by
		// their immutable manifest and therefore update the Page Directory alone.
		[[nodiscard]] SharedPagePoolAllocation evacuatePage(PageId pageId, SegmentId destinationSegmentId,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &publishOptions = {});
		// Builds a retirement entry from the current physical owner header. This is
		// maintenance-only because a relocated LargeValue frame has a physical owner
		// LSN that intentionally differs from the value's logical manifest LSN.
		[[nodiscard]] PageRetirement planPageRetirement(const MetadataSnapshotPin &snapshot, PageId pageId,
														LargeValueId valueId, PageOwnerKind ownerKind) const;

	private:
		friend class PropertyCollectionRootStore;

		// The metadata transaction allocates and validates the identity fields
		// before invoking its typed Property-store preparer. The allocator fills the
		// prepared frame locations in this result; the transaction revalidates every
		// identity field before publication.
		using PoolPreparationWriter = std::function<void(const CatalogEntry &, bool, size_t,
				std::span<const SharedPagePoolPreparedAppend>, std::span<PageLocation>)>;
		void prepareNativeGraphMaterialization(const NativeGraphLargeValueMutation &mutation,
											   LargeValueMaterializationResult &materialization,
											   LogSequenceNumber checkpointLsn,
											   const PoolPreparationWriter &writer = {});
		void prepareNativeGraphMaterializations(std::span<const NativeGraphLargeValueMutation> mutations,
				std::span<LargeValueMaterializationResult> materializations, LogSequenceNumber checkpointLsn);
		[[nodiscard]] SharedPagePoolAllocation evacuateManifestExpected(MetadataSnapshotPin &snapshot,
																		const LargeValueManifestLocation &expected,
																		SegmentId destinationSegmentId,
																		LogSequenceNumber checkpointLsn,
																		const MetadataPublishOptions &publishOptions);
		[[nodiscard]] SharedPagePoolAllocation evacuateDataExpected(MetadataSnapshotPin &snapshot, PageId pageId,
																	LargeValueId valueId,
																	SegmentId destinationSegmentId,
																	LogSequenceNumber checkpointLsn,
																	const MetadataPublishOptions &publishOptions);
		[[nodiscard]] LargeValueManifest readManifest(const MetadataSnapshotPin &snapshot,
													  const LargeValueRef &reference,
				const std::function<void(size_t)> &bufferAdmission = {}) const;
		[[nodiscard]] LargeValueManifest readManifestNode(const MetadataSnapshotPin &snapshot,
												  const LargeValueManifest &root, PageId pageId,
												  BufferPoolReadHint hint,
				const std::function<void(size_t)> &bufferAdmission = {}) const;
		[[nodiscard]] utils::OwnedVector<std::byte> readRawChunk(const MetadataSnapshotPin &snapshot,
												  const LargeValueManifest &manifest,
												  const LargeValueChunk &chunk, BufferPoolReadHint firstPageHint,
				const std::function<void(size_t)> &bufferAdmission = {}) const;
		void prepareRetirements(std::span<const ExistingPoolPageRetirementBatch> batches,
								LogSequenceNumber checkpointLsn) const;

		[[nodiscard]] std::vector<std::byte> readRangeFromManifestImpl(const MetadataSnapshotPin &snapshot,
			const LargeValueManifest &manifest, uint64_t offset, uint64_t length,
			const std::function<void(size_t)> &bufferAdmission, RangeReadCache *cache) const;
		SharedPagePoolAllocator allocator_;
		RelocatableMetadataStore &metadata_;
		std::unique_ptr<SlottedPagePool> content_index_pages_;
		std::unique_ptr<BTreePageStore> content_index_;
		std::shared_ptr<StorageIO> io_;
	};

} // namespace graph::storage
