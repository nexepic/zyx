/**
 * @file SharedPagePoolAllocator.hpp
 * @brief Atomic initial-page allocation for durable relocatable shared page pools.
 *
 * The allocator creates a pool segment and its first logical page in one
 * metadata generation. It does not allocate Page IDs or interpret page
 * payloads; typed stores own both policies above this physical boundary.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include "graph/utils/OwnedVector.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	class SlottedPagePool;
	class EntityLabelSetStore;
	class EntityPageStore;
	class VectorAdjacencyPageStore;
	class VectorManifestStore;
	class LargeValueStore;
    class HybridPropertyFieldPageStore;

	// Prefixes avoid Windows macro collisions. Read-only consumers such as an
	// integrity scanner must not repair or rewrite pool metadata while inspecting
	// a pinned snapshot.
	enum class SharedPagePoolOpenMode : uint8_t {
		SPPOM_RECONCILE = 1,
		SPPOM_READ_ONLY = 2,
		// Attach without a database-wide scan. Allocation and reclamation reconcile
		// the selected pool before it can reuse a frame, while reads authenticate
		// every routed frame on demand.
		SPPOM_DEFER_RECONCILIATION = 3,
	};

	struct SharedPagePoolAllocationRequest {
		SegmentId segment_id{};
		SegmentKind pool_kind = SegmentKind::SGK_NONE;
		uint64_t pool_owner_id = 0;
		Generation generation{};
		PageId page_id{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
		uint32_t page_free_bytes = 0;
		// A request owns its bounded page image so metadata publication callbacks
		// cannot retain a view into a temporary codec frame.
		std::vector<std::byte> payload;
	};

	struct SharedPagePoolAllocation {
		Superblock superblock;
		CatalogEntry pool;
		PageLocation location;
		PageOwnerHeader owner;
	};

	struct SharedPagePoolAppendRequest {
		SegmentId segment_id{};
		Generation generation{};
		PageId page_id{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
		uint32_t page_free_bytes = 0;
		std::vector<std::byte> payload;
	};

	struct SharedPagePoolReplacementRequest {
		SegmentId segment_id{};
		Generation generation{};
		PageId page_id{};
		LogSequenceNumber expected_owner_lsn{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
		uint32_t page_free_bytes = 0;
		std::vector<std::byte> payload;
	};

	struct SharedPagePoolEvacuationRequest {
		SegmentId source_segment_id{};
		Generation source_generation{};
		SegmentId destination_segment_id{};
		Generation destination_generation{};
		PageId page_id{};
		LogSequenceNumber expected_owner_lsn{};
		uint64_t page_owner_id = 0;
		PageOwnerKind page_owner_kind = PageOwnerKind::POK_NONE;
		uint32_t page_free_bytes = 0;
		std::vector<std::byte> payload;
	};

	struct SharedPagePoolContents {
		CatalogEntry pool;
		PageLocation location;
		SharedPagePoolHeader pool_header;
		PageOwnerHeader owner;
		utils::OwnedVector<std::byte> payload;
	};

	// A read-only validated page view retains the Buffer Pool pin that owns its
	// bytes. It is storage-internal and intentionally exposes no physical route
	// outside typed storage code.
	struct SharedPagePoolPinnedView {
		CatalogEntry pool;
		PageLocation location;
		SharedPagePoolHeader pool_header;
		PageOwnerHeader owner;
		BufferPool::Pin pin;

		SharedPagePoolPinnedView(CatalogEntry poolValue, PageLocation locationValue,
											 SharedPagePoolHeader poolHeaderValue, PageOwnerHeader ownerValue,
											 BufferPool::Pin pinValue) :
				pool(std::move(poolValue)), location(std::move(locationValue)), pool_header(std::move(poolHeaderValue)),
				owner(std::move(ownerValue)), pin(std::move(pinValue)) {}

		SharedPagePoolPinnedView(SharedPagePoolPinnedView &&) noexcept = default;
		SharedPagePoolPinnedView &operator=(SharedPagePoolPinnedView &&) noexcept = default;
		SharedPagePoolPinnedView(const SharedPagePoolPinnedView &) = delete;
		SharedPagePoolPinnedView &operator=(const SharedPagePoolPinnedView &) = delete;

		[[nodiscard]] std::span<const std::byte> payload() const {
			return pin.bytes().subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes);
		}
	};

	// Prepared typed-page bytes that are durable before a caller publishes the
	// matching Page Directory entries. This stays below B+Tree/Property codecs.
	struct SharedPagePoolPreparedAppend {
		PageOwnerHeader owner;
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0;
	};

	// A replacement carries the source owner's LSN so a batch cannot overwrite a
	// page whose current frame changed after the B+Tree mutation was planned.
	// `owner` describes the new reachable frame and therefore uses the batch LSN.
	struct SharedPagePoolPreparedReplacement {
		PageOwnerHeader owner;
		LogSequenceNumber expected_owner_lsn{};
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0;
	};

	// A retired frame is removed from the current Page Directory in the same
	// metadata publication. Its owner remains allocated and marked retired until
	// all snapshots that can resolve its old directory root have drained.
	struct SharedPagePoolPreparedRetirement {
		PageLocation location;
		uint64_t expected_owner_id = 0;
		PageOwnerKind expected_owner_kind = PageOwnerKind::POK_NONE;
		LogSequenceNumber expected_owner_lsn{};
	};

	// A metadata coordinator uses this bounded post-write proof before it makes
	// new Page Directory routes durable. The typed writer has already selected
	// the payload; this only authenticates the durable pool-control and owner
	// headers for the exact frames it was assigned.
	struct SharedPagePoolPreparedPublication {
		PageLocation location;
		uint64_t expected_owner_id = 0U;
		PageOwnerKind expected_owner_kind = PageOwnerKind::POK_NONE;
		LogSequenceNumber expected_owner_lsn{};
	};

	// A graph transaction may COW an Entity page into another compatible pool.
	// The source is retained for old snapshots while the destination receives
	// the new Page Directory location in the same metadata publication.
	struct SharedPagePoolPreparedCrossPoolReplacement {
		CatalogEntry source_pool;
		PageLocation source_location;
		LogSequenceNumber expected_owner_lsn{};
		PageOwnerHeader owner;
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0U;
	};

	class SharedPagePoolAllocator final {
		friend class SlottedPagePool;
		friend class EntityLabelSetStore;
		friend class EntityPageStore;
		friend class RelocatableMetadataStore;
        friend class HybridPropertyPoolPlacement;
		friend class SharedPagePoolAllocatorTestAccess;
		friend class VectorAdjacencyPageStore;
		friend class VectorPageStore;
		friend class VectorManifestStore;
		friend class LargeValueStore;
        friend class HybridPropertyFieldPageStore;
        friend class HybridPropertyReadContext;
        friend class RelocatableIntegrityScanner;

	public:
		// `io` must be the same data-file handle used by `metadata`, otherwise a
		// successful Catalog publication could reference another file.
		SharedPagePoolAllocator(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
								SharedPagePoolOpenMode openMode = SharedPagePoolOpenMode::SPPOM_RECONCILE);

		[[nodiscard]] std::optional<SharedPagePoolAllocation>
		allocateNewPoolPage(SharedPagePoolAllocationRequest request, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
							LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		[[nodiscard]] SharedPagePoolAllocation appendPage(SharedPagePoolAppendRequest request,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});

		// Replaces a published page through a Page Directory COW root switch.
		// The old frame is marked retired and remains readable through snapshots;
		// call `reclaimRetiredFrames` after releasing snapshots to reuse it.
		[[nodiscard]] SharedPagePoolAllocation replacePage(SharedPagePoolReplacementRequest request,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options = {});

		// Copies a Page ID into a different shared pool and publishes its new
		// location in one Page Directory COW generation. The old source frame stays
		// retired until no active metadata snapshot can observe it.
		[[nodiscard]] SharedPagePoolAllocation evacuatePage(SharedPagePoolEvacuationRequest request,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});

		// BTreePageStore supplies a rebuilt typed payload and, for root pages, the
		// matching Index Root generation rebind. The page bytes are durable before
		// the combined Page/Index Root metadata publication becomes recoverable.
		[[nodiscard]] SharedPagePoolAllocation
		evacuateIndexPage(SharedPagePoolEvacuationRequest request, const std::optional<IndexRootLocation> &expectedRoot,
						  const std::optional<IndexRootLocation> &replacementRoot, LogSequenceNumber checkpointLsn,
						  const MetadataPublishOptions &options = {});
		[[nodiscard]] SharedPagePoolAllocation
		evacuateVectorManifestPage(SharedPagePoolEvacuationRequest request,
								   const VectorManifestLocation &expectedManifest,
								   const VectorManifestLocation &replacementManifest, LogSequenceNumber checkpointLsn,
								   const MetadataPublishOptions &options = {});
		// LargeValue Manifest pages have an additional logical directory route. The
		// typed operation keeps that route and the Page Directory location in one
		// publication while preserving the value's logical generation.
		[[nodiscard]] SharedPagePoolAllocation
		evacuateLargeValueManifestPage(SharedPagePoolEvacuationRequest request,
									   const LargeValueManifestLocation &expectedManifest,
									   const LargeValueManifestLocation &replacementManifest,
									   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		// The caller keeps `snapshot` pinned for the entire lifetime of the
		// returned bytes, so delayed reuse cannot overwrite the referenced pool.
		// Sequential reads keep the shared pool-control frame hot but make only the
		// owned data frame transient, preventing typed scans from displacing the
		// point-read working set.
		[[nodiscard]] SharedPagePoolContents read(const MetadataSnapshotPin &snapshot, PageId pageId,
												  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Keeps the owned data frame pinned after validating the Page Directory,
		// Catalog, pool control record, and page owner. A disabled Buffer Pool
		// or a one-frame sequential scan returns nullopt, preserving a bounded
		// copy-based path without holding the only frame across consumer callbacks.
		[[nodiscard]] std::optional<SharedPagePoolPinnedView>
		readPinned(const MetadataSnapshotPin &snapshot, PageId pageId,
					   BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// This is a placement hint only. A caller still prepares its complete
		// mutation under the metadata writer, which repeats the authoritative
		// frame-capacity check before any directory root is published.
		[[nodiscard]] uint8_t availableFrameCount(const CatalogEntry &pool) const;

		// Reconciles one current pool after open or a failed publication. It clears
		// only frames whose valid owner header has no matching Page Directory
		// mapping; malformed headers and mismatched locations fail closed.
		void reconcilePool(SegmentId segmentId) const;
		void reclaimRetiredFrames(SegmentId segmentId) const;
		// Retires a current pool only after reconciliation and an atomic metadata
		// check prove that no current Page Directory entry names it.
		[[nodiscard]] Superblock retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &options = {});
		// Releases an already retired empty pool after all process-local metadata
		// snapshots have stopped resolving its previous Catalog generation.
		[[nodiscard]] Superblock releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});
        [[nodiscard]] std::optional<Superblock> tryReleaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});

	private:
        // A typed reader may attach to an already authenticated pin while the
        // metadata writer is held. Do not acquire another snapshot here.
        SharedPagePoolAllocator(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
            const MetadataSnapshotPin &snapshot) : io_(std::move(io)), metadata_(metadata) {
            if (!io_ || !metadata_.usesDataStorage(io_) || !metadata_.ownsSnapshot(snapshot))
                throw FormatException(FormatError::FERR_INVALID_STATE, "typed pool attachment requires its own file and live pin");
            data_region_offset_ = snapshot.superblock().data_region_offset;
        }
		// Typed scans may already hold a Page Directory location and Catalog entry
		// authenticated from the same pinned snapshot. Reuse that route without
		// repeating logical-tree lookups; physical pool and owner validation remains
		// identical to the public PageId-based read path.
		[[nodiscard]] SharedPagePoolContents readKnownLocation(
				const MetadataSnapshotPin &snapshot, const CatalogEntry &pool, const PageLocation &location,
				BufferPoolReadHint hint, bool hybridFieldOnly = false) const;
		[[nodiscard]] std::optional<SharedPagePoolPinnedView> readPinnedKnownLocation(
				const MetadataSnapshotPin &snapshot, const CatalogEntry &pool, const PageLocation &location,
				BufferPoolReadHint hint, bool hybridFieldOnly = false) const;

		// Typed page stores use this only after decoding a checked next-page route.
		// It resolves the current snapshot mapping again before touching the cache,
		// so a logical PageId is never treated as a physical address.
		void prefetch(const MetadataSnapshotPin &snapshot, std::span<const PageId> pageIds,
					  BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH) const;
		// The typed SlottedPagePool calls this only from a metadata writer callback.
		// It writes all requested frames before the enclosing COW roots become
		// recoverable, and leaves failed unpublished frames for reconciliation.
		void prepareAppendPages(const CatalogEntry &pool, std::span<const SharedPagePoolPreparedAppend> pages,
								std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) const;
		// Initializes a freshly reserved pool and all of its initial frames before
		// the enclosing Catalog/Page Directory publication can become recoverable.
		// A compound writer may materialize disjoint suffixes of one newly reserved
		// pool; `preparedPrefixCount` then fences the already durable initial frames.
		void prepareNewPoolPages(const CatalogEntry &pool, std::span<const SharedPagePoolPreparedAppend> pages,
								 std::span<PageLocation> locations, LogSequenceNumber checkpointLsn,
								 size_t preparedPrefixCount = 0U) const;
		// A metadata coordinator that already owns the writer fence uses this form
		// to initialize candidate pool frames without opening a nested snapshot.
		// It has identical validation and write ordering to prepareNewPoolPages().
		static void prepareNewPoolPagesUnderWrite(StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
												  std::span<const SharedPagePoolPreparedAppend> pages,
												  std::span<PageLocation> locations, LogSequenceNumber checkpointLsn,
												  size_t preparedPrefixCount = 0U, bool nativeHybridFields = false);
		// Appends to a current pool while the enclosing metadata coordinator already
		// owns the writer fence. This mirrors prepareAppendPages() without opening
		// a nested snapshot or publishing an intermediate directory root.
		static void prepareAppendPagesUnderWrite(StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
													 std::span<const SharedPagePoolPreparedAppend> pages,
													 std::span<PageLocation> locations, LogSequenceNumber checkpointLsn, bool nativeHybridFields = false);
		// Writes a group of replacement and appended frames while a single metadata
		// writer owns the current Page Directory root. All supplied frames are
		// durable before the caller switches that root, preserving snapshot COW.
		void prepareMutationPages(const CatalogEntry &pool, std::span<const PageLocation> expectedReplacements,
								  std::span<const SharedPagePoolPreparedReplacement> replacements,
								  std::span<const SharedPagePoolPreparedAppend> appends,
								  std::span<PageLocation> replacementLocations, std::span<PageLocation> appendLocations,
								  LogSequenceNumber checkpointLsn,
								  std::span<const SharedPagePoolPreparedRetirement> retirements = {}) const;
		// Authenticates retirement sources before a compound writer prepares any
		// replacement data. This keeps an invalid typed owner from leaving an
		// unreachable candidate suffix merely to discover a stale plan.
		void validateRetirementSources(const CatalogEntry &pool,
									   std::span<const SharedPagePoolPreparedRetirement> retirements) const;
		// Writer-safe form for metadata coordinators that already hold the writer
		// guard and therefore must not acquire a nested metadata snapshot.
		static void validateRetirementSourcesUnderWrite(const StorageIO &io, uint64_t dataRegionOffset,
														const CatalogEntry &pool,
														std::span<const SharedPagePoolPreparedRetirement> retirements);
		// Writer-safe post-materialization validation. It reads only the newly
		// assigned frames, never the global Page Directory or unrelated pool data.
		static void
		validatePreparedPublicationsUnderWrite(const StorageIO &io, uint64_t dataRegionOffset, const CatalogEntry &pool,
											   std::span<const SharedPagePoolPreparedPublication> publications);
		// Writes COW destinations in one current target pool. Sources may belong
		// to other compatible pools, which lets a full source remain readable by
		// old snapshots without imposing a per-pool write-size ceiling.
		void prepareCrossPoolReplacementPages(const CatalogEntry &destinationPool,
											  std::span<const SharedPagePoolPreparedCrossPoolReplacement> replacements,
											  std::span<PageLocation> replacementLocations,
											  LogSequenceNumber checkpointLsn) const;
		void validateRequest(const SharedPagePoolAllocationRequest &request, LogSequenceNumber checkpointLsn) const;
		void validateAppendRequest(const SharedPagePoolAppendRequest &request, const CatalogEntry &pool,
								   LogSequenceNumber checkpointLsn) const;
		void validateReplacementRequest(const SharedPagePoolReplacementRequest &request, const CatalogEntry &pool,
										LogSequenceNumber checkpointLsn) const;
		void validateEvacuationRequest(const SharedPagePoolEvacuationRequest &request, const CatalogEntry &sourcePool,
									   const CatalogEntry &destinationPool, LogSequenceNumber checkpointLsn) const;
		[[nodiscard]] SharedPagePoolAllocation
		evacuatePageImpl(SharedPagePoolEvacuationRequest request, const std::optional<IndexRootLocation> &expectedRoot,
						 const std::optional<IndexRootLocation> &replacementRoot,
						 const std::optional<VectorManifestLocation> &expectedManifest,
						 const std::optional<VectorManifestLocation> &replacementManifest,
						 const std::optional<LargeValueManifestLocation> &expectedLargeValueManifest,
						 const std::optional<LargeValueManifestLocation> &replacementLargeValueManifest,
						 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options);
		void writePreparedPool(const CatalogEntry &entry, const PageOwnerHeader &owner,
							   std::span<const std::byte> payload) const;
		[[nodiscard]] SharedPagePoolHeader readPoolHeader(const CatalogEntry &entry) const;
		// Compound metadata publication already owns the writer fence and cannot
		// construct an allocator that reacquires a snapshot. Reuse the same complete
		// typed-page validation without opening another reader pin.
		[[nodiscard]] static uint8_t availableFrameCountUnderWrite(const StorageIO &io, uint64_t dataRegionOffset,
																   const CatalogEntry &pool);
		[[nodiscard]] static SharedPagePoolHeader readPoolHeader(const StorageIO &io, uint64_t dataRegionOffset,
																 const CatalogEntry &entry);
		void reconcileCurrentPools() const;
		[[nodiscard]] bool
		reconcilePoolFrames(const CatalogEntry &entry,
							const std::function<std::optional<PageLocation>(PageId)> &findPageLocation,
							const std::function<CatalogEntry(SegmentId)> &findCatalog,
							const ExistingPoolFrameVisibility &isFrameVisibleToActiveSnapshot,
                            bool nativeHybridFields = false) const;
		[[nodiscard]] bool reconcilePoolFramesAtStartup(const CatalogEntry &entry,
														std::span<const PageLocation> pageLocations) const;
		void writePreparedPage(const CatalogEntry &entry, const SharedPagePoolHeader &poolHeader,
							   const PageOwnerHeader &owner, std::span<const std::byte> payload,
							   uint8_t frameIndex) const;
		[[nodiscard]] PageOwnerHeader readPageOwner(const CatalogEntry &entry, uint8_t frameIndex) const;
        [[nodiscard]] PageOwnerHeader readRecoveryPageOwner(const CatalogEntry &entry, uint8_t frameIndex, uint64_t fileBytes) const;
		void writePreparedReplacement(const CatalogEntry &entry, const SharedPagePoolHeader &poolHeader,
									  const PageOwnerHeader &oldOwner, uint8_t oldFrame,
									  const PageOwnerHeader &newOwner, std::span<const std::byte> payload,
									  uint8_t newFrame) const;
		void writePreparedCrossPoolEvacuation(const CatalogEntry &sourcePool, const SharedPagePoolHeader &sourceHeader,
											  const PageOwnerHeader &sourceOwner, uint8_t sourceFrame,
											  const CatalogEntry &destinationPool,
											  const SharedPagePoolHeader &destinationHeader,
											  const PageOwnerHeader &destinationOwner,
											  std::span<const std::byte> payload, uint8_t destinationFrame) const;
		std::shared_ptr<StorageIO> io_;
		RelocatableMetadataStore &metadata_;
		uint64_t data_region_offset_ = 0;
	};

} // namespace graph::storage
