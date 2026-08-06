/**
 * @file BTreePageStore.hpp
 * @brief Durable materialization and COW replacement of relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	class DerivedObjectReachabilityReclaimer;
	class ScalarIndexTreeReachabilityReclaimer;
	class TransactionBTreeOverlay;

	// A validated B+Tree page scheduled for snapshot-safe retirement. The
	// neutral type serves both vector-derived mappings and public scalar trees.
	struct RetiredIndexTreePage {
		PageLocation location;
		uint64_t index_id = 0;
		LogSequenceNumber owner_lsn{};

		constexpr bool operator==(const RetiredIndexTreePage &) const = default;
	};

	// The database layer owns physical capacity policy. B+Tree growth receives
	// an explicit range instead of guessing from file offsets, while metadata
	// assigns the stable logical Segment ID in the same publication batch.
	struct IndexPagePoolGrowthOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t pool_owner_id = 0;
	};

	// The bulk builder supplies one physical reservation range per new pool.
	// `page_count` is deliberately capped to one shared-page segment; a tree can
	// use any number of these pools without making a physical address durable.
	struct IndexTreeMaterializationPoolOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t pool_owner_id = 0;
		size_t page_count = 0;
	};

	struct IndexTreeMaterializedPool {
		CatalogEntry pool;
		std::span<const PageId> page_ids;
	};

	struct IndexTreeMaterializedPageReference {
		size_t pool_ordinal = 0;
		size_t page_ordinal = 0;
	};

	struct BTreeNewIndexTreePoolMutation {
		uint64_t owner_id = 0;
		size_t appended_page_count = 0;
	};

	// Operations remain logical until one transaction-local B+Tree overlay has
	// replayed the complete ordered stream. This prevents a later structural
	// change from being planned against a stale, pre-transaction tree shape.
	// Prefixes keep the enum safe from Windows headers.
	enum class BTreeScalarIndexOperationKind : uint8_t {
		BSIOP_UPSERT = 1,
		BSIOP_ERASE = 2,
	};

	struct BTreeScalarIndexOperation {
		BTreeScalarIndexOperationKind kind = BTreeScalarIndexOperationKind::BSIOP_UPSERT;
		std::vector<std::byte> key;
		uint64_t value = 0U;
	};

	using BTreeStructuralMutationPreparer =
			std::function<void(std::vector<IndexTreeMutationPoolBatch> &, LogSequenceNumber)>;

	// Initial B+Tree values can depend on another typed object that receives its
	// PageId from the same metadata batch. The binder runs only after those
	// metadata-owned PageIds are assigned; it can replace leaf values but cannot
	// alter keys or the pre-planned tree layout.
	struct BTreeInitialTreeValueBindingContext {
		std::span<const IndexTreeMutationPoolBatch> pool_batches;
		std::span<const PageId> page_ids;
		std::span<const Generation> generations;
	};

	using BTreeInitialTreeValueBinder =
			std::function<void(std::span<BTreePageEntry>, const BTreeInitialTreeValueBindingContext &)>;

	// A fresh immutable tree may either introduce a previously absent IndexId or
	// replace one that is retired by the same enclosing metadata publication.
	// Prefixes keep the enum safe from Windows headers.
	enum class BTreeFreshTreeRootMode : uint8_t {
		BFTM_REQUIRE_ABSENT = 1,
		BFTM_REQUIRE_PRESENT = 2,
	};

	// A fresh immutable tree has no source pages to rewrite, but it still follows
	// the same metadata-assigned PageId and single-publication protocol as a COW
	// mutation. `BFTM_REQUIRE_PRESENT` is only valid when the enclosing metadata
	// plan retires that current root in its same generation.
	struct BTreeInitialTreeMutationPlan {
		uint64_t index_id = 0U;
		std::vector<BTreeNewIndexTreePoolMutation> new_pool_mutations;
		IndexTreeMutationAppendedPageReference root_page;
		BTreeStructuralMutationPreparer preparer;
	};

	// One immutable structural tree plan. It carries the complete Page Directory
	// mutation shape and one data-preparation callback, but publishes nothing by
	// itself. Slot selection and the durable checkpoint LSN are supplied only by
	// the enclosing metadata publication, so graph writes cannot derive either
	// identity from a standalone tree commit.
	struct BTreeStructuralMutationPlan {
		IndexRootLocation expected_root;
		std::vector<IndexTreeMutationPoolRequest> pool_requests;
		// The first fresh pool remains separate for source compatibility with the
		// original single-pool structural planners. Transaction-local overlays may
		// require additional bounded pools when one logical mutation appends more
		// than a shared pool can physically contain.
		std::optional<BTreeNewIndexTreePoolMutation> new_pool_mutation;
		std::vector<BTreeNewIndexTreePoolMutation> additional_new_pool_mutations;
		std::optional<IndexTreeMutationAppendedPageReference> replacement_root;
		std::optional<PageId> stable_replacement_root;
		BTreeStructuralMutationPreparer preparer;
		// Generic scalar indexes may intentionally retain an empty root. Callers
		// that require root absence can use this verified terminal-state signal to
		// substitute a bounded whole-tree retirement in their enclosing commit.
		bool final_tree_empty = false;
	};

	// An explicit whole-tree retirement plan. The plan is generated from one
	// pinned root and contains every reachable page, so a caller can remove the
	// root mapping and retire its frames in one metadata generation.
	struct BTreeTreeRetirementPlan {
		IndexRootLocation expected_root;
		std::vector<IndexTreeMutationPoolRequest> pool_requests;
		BTreeStructuralMutationPreparer preparer;
	};

	// A non-structural root-leaf update expressed without publishing metadata.
	// The plan retains every current identity that a larger graph mutation must
	// fence under its writer before it makes the replacement frame reachable.
	struct BTreeRootLeafMutationPlan {
		IndexRootLocation expected_root;
		CatalogEntry expected_pool;
		PageLocation expected_location;
		LogSequenceNumber expected_owner_lsn{};
		std::vector<std::byte> replacement_payload;
		uint32_t replacement_free_bytes = 0;
	};

	class BTreePageStore final {
	public:
		using NewTreePageFactory = std::function<std::vector<BTreePage>(const CatalogEntry &, std::span<const PageId>)>;
		// Receives the complete logical Page-ID assignment, then produces one page
		// at a time. This lets a large bottom-up builder link pages in different
		// pools without retaining all serialized page images in memory.
		using MultiPoolNewTreePageFactory = std::function<BTreePage(std::span<const IndexTreeMaterializedPool>,
																	size_t poolOrdinal, size_t pageOrdinal)>;

		BTreePageStore(SlottedPagePool &pages, RelocatableMetadataStore &metadata);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<SlottedPagePoolAllocation>
		allocate(const SlottedPagePoolAllocationRequest &request, const BTreePage &page, PhysicalSlotId firstSlot,
				 PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] SharedPagePoolAllocation append(const SlottedPagePoolAppendRequest &request,
													  const BTreePage &page, LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		[[nodiscard]] BTreePage read(const MetadataSnapshotPin &snapshot, PageId pageId,
									 BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Plans an upsert that fits in an existing unsplit root leaf. It never
		// publishes or writes a frame. Structural splits intentionally remain on
		// the full tree-mutation path until their complete plan can join graph COW.
		[[nodiscard]] BTreeRootLeafMutationPlan planRootLeafUpsert(const MetadataSnapshotPin &snapshot,
																   uint64_t indexId, std::span<const std::byte> key,
																   uint64_t value) const;
		// Low-level metadata clients may request a fully fenced deep structural
		// plan without preparing frames or publishing it. Transaction commits use
		// the ordered overlay API below to aggregate all operations per Index.
		[[nodiscard]] BTreeStructuralMutationPlan planDeepStructuralUpsert(const MetadataSnapshotPin &snapshot,
																		   uint64_t indexId,
																		   std::span<const std::byte> key,
																		   uint64_t value) const;
		// Replays one Index's ordered logical operations through a private COW
		// overlay and returns exactly one final metadata COW publication plan.
		// No physical frames or logical Page IDs are allocated while planning. When
		// `rebuild_on_frame_exhaustion` is true, an exhausted source pool is
		// rematerialized into fresh immutable pools instead of failing a valid
		// structural mutation solely because it lacks COW frames.
		[[nodiscard]] BTreeStructuralMutationPlan
		planScalarIndexTransactionMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
											   std::span<const BTreeScalarIndexOperation> operations,
											   bool rebuild_on_frame_exhaustion = false,
											   bool force_fresh_rebuild = false,
											   BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) const;
		// Traverses one current tree under a caller-supplied bound and produces the
		// typed page-retirement callback required by a compound metadata commit.
		// It never publishes a root deletion on its own.
		[[nodiscard]] BTreeTreeRetirementPlan planTreeRetirement(const MetadataSnapshotPin &snapshot, uint64_t indexId,
																 size_t maximumPages) const;
		// Plans first publication of one bounded, already ordered tree. The
		// metadata writer rechecks root absence immediately before publication,
		// so planning never makes an IndexId visible on its own. When provided,
		// `valueBinder` may resolve zero placeholder leaf values from identities
		// assigned by that same metadata transaction; it may not alter keys.
		[[nodiscard]] BTreeInitialTreeMutationPlan
		planInitialTreeMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
								std::span<const BTreePageEntry> entries,
								BTreeInitialTreeValueBinder valueBinder = {},
								BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) const;
		// Plans a fresh replacement tree while requiring one current root. The
		// caller must retire that root and every reachable source page in the same
		// metadata generation that publishes this tree.
		[[nodiscard]] BTreeInitialTreeMutationPlan
		planReplacementTreeMutation(const MetadataSnapshotPin &snapshot, uint64_t indexId,
									std::span<const BTreePageEntry> entries,
									BTreeInitialTreeValueBinder valueBinder = {},
									BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) const;
		[[nodiscard]] BTreeRootLeafMutationPlan planRootLeafErase(const MetadataSnapshotPin &snapshot, uint64_t indexId,
																  std::span<const std::byte> key) const;
		// Materializes the candidate frame for an already validated root-leaf plan.
		// It deliberately does not publish metadata; the graph mutation supplies the
		// enclosing Page Directory and COW generation.
		void prepareRootLeafMutation(const BTreeRootLeafMutationPlan &plan, const CatalogEntry &pool,
									 const PageLocation &source, PageLocation &replacement,
									 LogSequenceNumber checkpointLsn) const;
		// Materializes several initial pages in one newly reserved Index pool.
		// The enclosing compound metadata batch publishes their Page Directory
		// locations and the selected Index root only after every frame is durable.
		void prepareNewPoolInitialPages(const CatalogEntry &pool, std::span<const BTreePage> pages,
										std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) const;
		// Checks the current durable frame map while the metadata writer owns the
		// pool identity. It lets a composite graph/index write return retryable
		// no-space before materializing any candidate frame.
		[[nodiscard]] bool hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const;
		// Reclaims only retired frames that no pinned metadata snapshot can still
		// resolve. Transaction planners call this before retrying a COW plan.
		void reclaimRetiredFrames(SegmentId segmentId) const;
		[[nodiscard]] SharedPagePoolAllocation replace(const BTreePage &page, LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		// Evacuates one typed B+Tree page into a compatible index pool. When the
		// page is the current root, its root generation is rebound atomically with
		// the Page Directory location so readers never observe a mixed identity.
		[[nodiscard]] SharedPagePoolAllocation evacuate(const BTreePage &page, SegmentId destinationSegmentId,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});
		// Only this typed store may publish an IndexId root mapping. It resolves
		// the candidate through the pinned Page Directory and proves the B+Tree
		// page's immutable owner before switching the metadata root.
		[[nodiscard]] Superblock publishRoots(std::span<const IndexRootLocation> roots,
											  std::span<const uint64_t> erasedIndexIds, LogSequenceNumber checkpointLsn,
											  const MetadataPublishOptions &options = {});
		// Allocates logical Page IDs, materializes all new B+Tree pages, and
		// publishes their Page Directory entries plus one new Index root in a
		// single Superblock-published metadata generation. `factory` receives every assigned
		// Page ID, allowing new child links to be encoded without a separate ID
		// allocation transaction.
		[[nodiscard]] Superblock materializeAndPublishTree(SegmentId poolSegmentId, uint64_t indexId, size_t pageCount,
														   size_t rootPageOrdinal, LogSequenceNumber checkpointLsn,
														   const NewTreePageFactory &factory,
														   const MetadataPublishOptions &options = {});
		// Materializes a new tree across several freshly reserved pools. All pages
		// stay unreachable until one Superblock-published metadata generation publishes the
		// Page Directory and Index Root Directory entries together.
		[[nodiscard]] Superblock materializeAndPublishTreeAcrossPools(
				std::span<const IndexTreeMaterializationPoolOptions> pools, uint64_t indexId,
				IndexTreeMaterializedPageReference rootPage, LogSequenceNumber checkpointLsn,
				const MultiPoolNewTreePageFactory &factory, const MetadataPublishOptions &options = {});
		// Establishes the mutation rule used by later split/merge work. A tree that
		// consists of one root leaf can be updated through a stable PageId COW
		// replacement: old snapshots retain the old frame while current readers see
		// the fully validated replacement. Full multi-level mutation is layered on
		// top of the tree-publication coordinator.
		[[nodiscard]] SharedPagePoolAllocation upsertRootLeaf(uint64_t indexId, std::span<const std::byte> key,
															  uint64_t value, LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options = {});
		[[nodiscard]] SharedPagePoolAllocation upsertRootLeaf(uint64_t indexId, std::span<const std::byte> key,
															  uint64_t value, IndexPagePoolGrowthOptions growth,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options = {});
		// The Stage 6 mutation entry point. It plans every replacement and appended
		// page across the affected pools, then publishes that complete COW mutation
		// through one metadata COW generation. Delete/merge work uses the same
		// multi-pool publication primitive with explicit retirement planning.
		[[nodiscard]] SharedPagePoolAllocation upsert(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		[[nodiscard]] SharedPagePoolAllocation upsert(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
													  IndexPagePoolGrowthOptions growth,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options = {});
		// Removes one key through a snapshot-pinned COW rewrite. The mutation
		// measures encoded page occupancy, redistributes or merges leaf/internal
		// siblings when representable, repairs leaf links, and collapses unary
		// roots in one multi-pool metadata publication; `false` means absent.
		[[nodiscard]] bool erase(uint64_t indexId, std::span<const std::byte> key, LogSequenceNumber checkpointLsn,
								 const MetadataPublishOptions &options = {});

	private:
		friend class DerivedObjectReachabilityReclaimer;
		friend class ScalarIndexTreeReachabilityReclaimer;
		friend class TransactionBTreeOverlay;
		[[nodiscard]] Superblock retireIndexTreePages(Generation expectedMetadataGeneration,
													  std::span<const IndexRootLocation> erasedRoots,
													  std::span<const RetiredIndexTreePage> pages,
													  LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] std::optional<SegmentId> selectAppendPool(const MetadataSnapshotPin &snapshot,
																std::span<const PageLocation> expectedReplacements,
																size_t appendedPageCount) const;
		[[nodiscard]] static NewIndexTreePoolRequest newPoolRequest(IndexPagePoolGrowthOptions growth, uint64_t indexId,
																	size_t appendedPageCount);
		[[nodiscard]] SharedPagePoolAllocation upsertRootLeafImpl(uint64_t indexId, std::span<const std::byte> key,
																  uint64_t value,
																  std::optional<IndexPagePoolGrowthOptions> growth,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options);
		[[nodiscard]] std::variant<SharedPagePoolAllocation, BTreeStructuralMutationPlan>
		upsertImpl(uint64_t indexId, std::span<const std::byte> key, uint64_t value,
				   std::optional<IndexPagePoolGrowthOptions> growth, LogSequenceNumber checkpointLsn,
				   const MetadataPublishOptions &options, bool planStructuralOnly = false,
				   const MetadataSnapshotPin *planningSnapshot = nullptr);
		[[nodiscard]] bool eraseImpl(uint64_t indexId, std::span<const std::byte> key, LogSequenceNumber checkpointLsn,
									 const MetadataPublishOptions &options);
		[[nodiscard]] BTreeInitialTreeMutationPlan planFreshTreeMutation(const MetadataSnapshotPin &snapshot,
																					 uint64_t indexId,
																					 std::span<const BTreePageEntry> entries,
																					 BTreeInitialTreeValueBinder valueBinder,
																					 BTreeFreshTreeRootMode rootMode,
																					 BTreeLeafValueLayout leafValueLayout) const;
		SlottedPagePool &pages_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
