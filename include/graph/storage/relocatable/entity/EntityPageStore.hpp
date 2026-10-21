/**
 * @file EntityPageStore.hpp
 * @brief Snapshot-safe Node and Edge persistence on fixed relocatable pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/EntityPageAuxiliaryMutation.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace graph::storage {

	class NativeReadSession;

	// A fresh pool receives its first populated Entity page and its Entity Map
	// entry in one data-before-metadata publication.
	struct EntityPageNewPoolRequest {
		SegmentId segment_id{};
		uint64_t pool_owner_id = 0;
		Generation generation{};
		PageId page_id{};
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
	};

	// An existing pool may hold multiple Node or Edge pages. The exact Catalog
	// entry is a generation/physical-slot fence against stale placement plans.
	struct EntityPageAppendRequest {
		CatalogEntry expected_pool;
		PageId page_id{};
	};

	struct EntityPageInsertResult {
		Superblock superblock;
		CatalogEntry pool;
		PageLocation page;
		EntityPageLocation location;
	};

	struct EntityPageMutationResult {
		Superblock superblock;
		CatalogEntry pool;
		PageLocation page;
		bool page_retired = false;
	};

	struct EntityPageNodeUpdate {
		uint64_t node_id = 0;
		NodeEntityRecord record;
	};

	struct EntityPageEdgeUpdate {
		uint64_t edge_id = 0;
		EdgeEntityRecord record;
	};

	struct EntityPageNodeInsertion {
		uint64_t node_id = 0;
		NodeEntityRecord record;
	};

	struct EntityPageEdgeInsertion {
		uint64_t edge_id = 0;
		EdgeEntityRecord record;
	};

	// A batch targets one currently published PageId. It updates, inserts, and
	// erases records in an in-memory COW image; EntityPageStore publishes every
	// page image and its matching Entity Map delta in one metadata generation.
	struct EntityPageBatchMutation {
		PageId page_id{};
		EntityPageRecordKind record_kind = EntityPageRecordKind::EPRK_NODE;
		std::vector<EntityPageNodeUpdate> node_updates;
		std::vector<EntityPageNodeInsertion> node_insertions;
		std::vector<uint64_t> erased_node_ids;
		std::vector<EntityPageEdgeUpdate> edge_updates;
		std::vector<EntityPageEdgeInsertion> edge_insertions;
		std::vector<uint64_t> erased_edge_ids;
		// The planner may route a COW image into another current Entity pool when
		// the source is full. An empty entry keeps the source pool as destination.
		CatalogEntry replacement_pool;
		// Used only when no compatible current Entity pool has an available COW
		// frame. Metadata reserves this destination in the same publication.
		std::optional<EntityPageFreshPoolDestination> replacement_new_pool;
	};

	// A batch append creates one fresh logical page in an already sealed Entity
	// pool. It is intentionally distinct from a page mutation: no current Page
	// Directory entry exists, so updates and erasures would be invalid here.
	struct EntityPageBatchAppend {
		EntityPageAppendRequest request;
		EntityPageRecordKind record_kind = EntityPageRecordKind::EPRK_NODE;
		std::vector<EntityPageNodeInsertion> node_insertions;
		std::vector<EntityPageEdgeInsertion> edge_insertions;
	};

	// A fresh physical Entity pool and its first logical page are staged with
	// other Entity mutations, rather than being published before the graph
	// topology that needs the new page.
	struct EntityPageBatchNewPool {
		EntityPageNewPoolRequest request;
		EntityPageRecordKind record_kind = EntityPageRecordKind::EPRK_NODE;
		std::vector<EntityPageNodeInsertion> node_insertions;
		std::vector<EntityPageEdgeInsertion> edge_insertions;
	};

	struct EntityPageBatchAppendResult {
		CatalogEntry pool;
		PageLocation page;
		std::vector<EntityPageLocation> locations;
	};

	struct EntityPageBatchResult {
		Superblock superblock;
		std::vector<EntityPageMutationResult> pages;
		std::vector<EntityPageBatchAppendResult> appended_pages;
		std::vector<EntityPageBatchAppendResult> new_pool_pages;
		// Only upserted owner-keyed roots are returned. Erased and unchanged roots
		// intentionally have no current Property-directory location.
		std::vector<EntityPropertyLocation> properties;
	};

	class EntityPageStore final {
	public:
		EntityPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata);
		EntityPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
						SharedPagePoolOpenMode openMode);

		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<EntityPageInsertResult>
		allocateNode(const EntityPageNewPoolRequest &request, uint64_t nodeId, const NodeEntityRecord &record,
					 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});
		[[nodiscard]] std::optional<EntityPageInsertResult>
		allocateEdge(const EntityPageNewPoolRequest &request, uint64_t edgeId, const EdgeEntityRecord &record,
					 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options = {});

		[[nodiscard]] EntityPageInsertResult appendNodePage(const EntityPageAppendRequest &request, uint64_t nodeId,
															const NodeEntityRecord &record,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageInsertResult appendEdgePage(const EntityPageAppendRequest &request, uint64_t edgeId,
															const EdgeEntityRecord &record,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options = {});

		// Inserting into a current page uses a 4 KiB COW frame. The supplied
		// snapshot is an optimistic placement fence; a concurrent replacement is
		// rejected instead of overwriting a newer page image.
		[[nodiscard]] EntityPageInsertResult insertNode(const MetadataSnapshotPin &snapshot, PageId pageId,
														uint64_t nodeId, const NodeEntityRecord &record,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageInsertResult insertEdge(const MetadataSnapshotPin &snapshot, PageId pageId,
														uint64_t edgeId, const EdgeEntityRecord &record,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options = {});

		[[nodiscard]] NodeEntityRecord readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const;
		[[nodiscard]] EdgeEntityRecord readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const;
		// Exposes a snapshot-pinned typed auxiliary page to the adjacency planner.
		// Callers must supply the expected owner kind, so this is not a generic raw
		// frame escape hatch.
		[[nodiscard]] SharedPagePoolContents
		readAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId, PageOwnerKind expectedOwnerKind,
						  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// A typed scan may warm a decoded successor while it consumes the current
		// page. This does not weaken the mandatory owner-kind validation at read.
		void prefetchAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId,
								   BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH) const;
		// These overloads consume one Entity Map entry already decoded by a
		// snapshot-pinned directory traversal. They keep native scans from doing a
		// second root-to-leaf lookup per record while still validating PageId, slot,
		// record kind, and durable slot generation before exposing a record.
		[[nodiscard]] NodeEntityRecord readNodeAt(const MetadataSnapshotPin &snapshot,
														  const EntityPageLocation &location) const;
		// Resolves and reads a strictly increasing Node-ID batch through one
		// directory range cursor, grouping locations by Entity page. Empty
		// optionals represent absent logical IDs; present records remain fully
		// authenticated by the typed page codec.
		[[nodiscard]] std::vector<std::optional<NodeEntityRecord>>
		readNodesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
			const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		[[nodiscard]] EdgeEntityRecord readEdgeAt(const MetadataSnapshotPin &snapshot,
														  const EntityPageLocation &location) const;
		// Resolves and reads a strictly increasing Edge-ID batch through one
		// directory range cursor, grouping locations by Entity page. Empty results
		// represent absent logical IDs; present records remain fully authenticated.
		// Optional admission covers live result/location/ordinal vector capacities.
		// Directory cursor and decoded Entity pages are accounted separately.
		[[nodiscard]] std::vector<std::optional<EdgeEntityRecord>>
		readEdgesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
			const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		// Ordinary scans walk current physical Entity pages and enumerate their live
		// slots without a per-record Entity Map lookup. Order is intentionally not a
		// contract; resumable logical-ID scans use visitNodesAfter below.
		[[nodiscard]] bool
		visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
						const std::function<bool(uint64_t nodeId, const NodeEntityRecord &)> &visitor,
						NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool
		visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
						const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor,
						NativeReadSession *session = nullptr) const;
		// The metadata-page limit bounds directory traversal independently from
		// result count, which preserves resumable sparse-ID scan behavior.
		[[nodiscard]] bool
		visitNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId, size_t maximumRecords,
						size_t maximumMetadataPages,
						const std::function<bool(uint64_t nodeId, const NodeEntityRecord &)> &visitor,
						size_t *visitedMetadataPages = nullptr) const;

		[[nodiscard]] EntityPageMutationResult updateNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														  const NodeEntityRecord &record,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageMutationResult updateEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
														  const EdgeEntityRecord &record,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options = {});
		// Atomically COW-replaces several current Entity pages and retires any page
		// whose final record is erased. Each Page Directory and Entity Map delta is
		// published in one metadata generation, so readers never observe a partial
		// graph topology.
		[[nodiscard]] EntityPageBatchResult applyBatch(const MetadataSnapshotPin &snapshot,
													   std::span<const EntityPageBatchMutation> mutations,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options = {});
		// Atomically combines COW updates to current pages with one or more fresh
		// Node/Edge pages in existing pools. This is the required primitive for a
		// graph topology mutation that needs both adjacency rewrites and a new Edge.
		[[nodiscard]] EntityPageBatchResult applyBatchWithAppends(const MetadataSnapshotPin &snapshot,
																  std::span<const EntityPageBatchMutation> mutations,
																  std::span<const EntityPageBatchAppend> appends,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options = {});
		// Complete Entity-page transaction primitive. The new-pool first page,
		// existing-pool appends, and current-page COW mutations share one durable
		// metadata generation, which keeps graph topology indivisible to readers.
		[[nodiscard]] EntityPageBatchResult applyBatchTransaction(const MetadataSnapshotPin &snapshot,
																  std::span<const EntityPageBatchMutation> mutations,
																  std::span<const EntityPageBatchAppend> appends,
																  std::span<const EntityPageBatchNewPool> newPools,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options = {});
		// Publishes typed Entity-pool auxiliary frames, currently only LabelSet
		// pages, with their bound Node COW updates. This lower boundary intentionally
		// has no Property-root argument so callers can compose a LabelSet plan before
		// introducing owner-keyed Property work.
		[[nodiscard]] EntityPageBatchResult applyAuxiliaryEntityBatchTransaction(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
				std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
				const EntityPageAuxiliaryMutationBatch &auxiliaryPages, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &options = {});
		// Extends the complete Entity-page transaction with snapshot-pinned,
		// owner-keyed Property root plans. Entity Map, Page Directory, Property
		// Directory, Catalog, and Allocator roots publish in one generation.
		[[nodiscard]] EntityPageBatchResult applyNativeGraphBatchTransaction(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
				std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
				std::span<const EntityPropertyRootMutationPlan> propertyPlans,
				const PropertyCollectionRootStore &propertyRoots,
				const PropertyCollectionRootStoreOptions &propertyOptions, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &options = {});
		// LabelSet and future typed auxiliary pages must publish with the Entity
		// mutation that makes their references reachable. This overload carries
		// only prevalidated Entity-pool frames; it never accepts untyped raw pages.
		[[nodiscard]] EntityPageBatchResult applyNativeGraphBatchTransaction(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
				std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
				std::span<const EntityPropertyRootMutationPlan> propertyPlans,
				const PropertyCollectionRootStore &propertyRoots,
				const PropertyCollectionRootStoreOptions &propertyOptions,
				const EntityPageAuxiliaryMutationBatch &auxiliaryPages, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &options = {});
		// Adds already-planned B+Tree derived state to the same Entity/Property
		// publication. The derived preparer receives only metadata-assigned pool
		// and Page identities, so index pages cannot be made visible ahead of the
		// graph state they describe.
		[[nodiscard]] EntityPageBatchResult applyNativeGraphBatchTransaction(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
				std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
				std::span<const EntityPropertyRootMutationPlan> propertyPlans,
				const PropertyCollectionRootStore &propertyRoots,
				const PropertyCollectionRootStoreOptions &propertyOptions,
				const EntityPageAuxiliaryMutationBatch &auxiliaryPages,
				const NativeGraphDerivedStateMutation &derivedState,
				const NativeGraphDerivedStatePreparer &derivedStatePreparer, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &options = {}, NativeGraphLogicalIdReservation logicalIdReservation = {},
				const NativeGraphVectorBootstrapPreparer &vectorBootstrapPreparer = {},
				const NativeGraphVectorDeltaPreparer &vectorDeltaPreparer = {},
				const NativeGraphPrivateSegmentImagePreparer &privateSegmentImagePreparer = {},
				// Private candidate data is scoped to the metadata writer and must never
				// escape into a public snapshot or logical transaction API.
				const NativeGraphPrivateSegmentPreparer &privateSegmentPreparer = {},
				const NativeGraphDeferredIndexPlanner &deferredIndexPlanner = {},
                const NativeGraphEarlyPublicationPlanner &earlyPublicationPlanner = {},
                const NativeHybridTopologyPublication *hybridTopology = nullptr);
		[[nodiscard]] EntityPageMutationResult eraseNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageMutationResult eraseEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageMutationResult evacuateNodePage(const MetadataSnapshotPin &snapshot, PageId pageId,
																SegmentId destinationSegmentId,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options = {});
		[[nodiscard]] EntityPageMutationResult evacuateEdgePage(const MetadataSnapshotPin &snapshot, PageId pageId,
																SegmentId destinationSegmentId,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options = {});
		// Entity pools can contain Node, Edge, and LabelSet auxiliary pages. This
		// dispatches by the durable owner kind and keeps each typed page codec on
		// its own validated relocation path.
		[[nodiscard]] EntityPageMutationResult evacuatePoolPage(const MetadataSnapshotPin &snapshot, PageId pageId,
																SegmentId destinationSegmentId,
																LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options = {});

		[[nodiscard]] uint16_t freeSlotCount(const MetadataSnapshotPin &snapshot, PageId pageId,
											 EntityPageRecordKind expectedKind) const;
		// A COW replacement or append consumes one free frame in its current pool.
		// Native graph planning reads this bounded physical fact before it builds a
		// batch, while metadata still repeats the authoritative check at publish.
		[[nodiscard]] uint8_t availableFrameCount(const MetadataSnapshotPin &snapshot,
												  const CatalogEntry &expectedPool) const;
		[[nodiscard]] std::vector<CatalogEntry> currentPools(const MetadataSnapshotPin &snapshot) const;
		void reclaimRetiredFrames(SegmentId segmentId) const;
		[[nodiscard]] Superblock retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &options = {});
		[[nodiscard]] Superblock releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});
        [[nodiscard]] std::optional<Superblock> tryReleaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options = {});

	private:
		struct LoadedPage {
			SharedPagePoolContents contents;
			EntityPage page;

			LoadedPage(SharedPagePoolContents contents, EntityPage page) :
				contents(std::move(contents)), page(std::move(page)) {}
		};

		// The point/scan read path keeps its data frame pinned and borrows its
		// validated payload. The owned fallback preserves direct-store tests that
		// intentionally run without a Buffer Pool.
		struct ReadPage {
			std::optional<SharedPagePoolPinnedView> pinned_contents;
			std::optional<LoadedPage> copied_contents;
			EntityPageReadView page;

			explicit ReadPage(SharedPagePoolPinnedView contents) :
					pinned_contents(std::move(contents)),
					page(EntityPageReadView::open(pinned_contents->owner, pinned_contents->payload())) {}
			explicit ReadPage(LoadedPage contents) :
					copied_contents(std::move(contents)),
					page(EntityPageReadView::open(copied_contents->contents.owner,
																	 copied_contents->contents.payload)) {}
		};

		[[nodiscard]] LoadedPage loadPage(const MetadataSnapshotPin &snapshot, PageId pageId,
										  EntityPageRecordKind expectedKind,
										  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		[[nodiscard]] ReadPage loadReadPage(const MetadataSnapshotPin &snapshot, PageId pageId,
													EntityPageRecordKind expectedKind,
													BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Entity Map order commonly groups records from the same Entity page. A
		// bounded scan retains only that current decoded page, so a transient read is
		// issued once per contiguous PageId run instead of once per Entity record.
		[[nodiscard]] LoadedPage &loadScanPage(const MetadataSnapshotPin &snapshot, PageId pageId,
											   EntityPageRecordKind expectedKind,
																	std::optional<LoadedPage> &currentPage, PageId &currentPageId) const;
		[[nodiscard]] ReadPage &loadScanReadPage(const MetadataSnapshotPin &snapshot, PageId pageId,
																		 EntityPageRecordKind expectedKind,
																		 std::optional<ReadPage> &currentPage,
																		 PageId &currentPageId) const;
		[[nodiscard]] std::optional<EntityPageInsertResult>
		allocate(const EntityPageNewPoolRequest &request, uint64_t entityId, EntityPageRecordKind kind,
				 const NodeEntityRecord *node, const EdgeEntityRecord *edge, LogSequenceNumber checkpointLsn,
				 const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageInsertResult append(const EntityPageAppendRequest &request, uint64_t entityId,
													EntityPageRecordKind kind, const NodeEntityRecord *node,
													const EdgeEntityRecord *edge, LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageInsertResult insert(const MetadataSnapshotPin &snapshot, PageId pageId,
													uint64_t entityId, EntityPageRecordKind kind,
													const NodeEntityRecord *node, const EdgeEntityRecord *edge,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageMutationResult update(const MetadataSnapshotPin &snapshot, uint64_t entityId,
													  EntityPageRecordKind kind, const NodeEntityRecord *node,
													  const EdgeEntityRecord *edge, LogSequenceNumber checkpointLsn,
													  const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageMutationResult erase(const MetadataSnapshotPin &snapshot, uint64_t entityId,
													 EntityPageRecordKind kind, LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageMutationResult evacuate(const MetadataSnapshotPin &snapshot, PageId pageId,
														SegmentId destinationSegmentId, EntityPageRecordKind kind,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageMutationResult evacuateAuxiliaryPage(const MetadataSnapshotPin &snapshot, PageId pageId,
																	 SegmentId destinationSegmentId,
																	 PageOwnerKind ownerKind,
																	 LogSequenceNumber checkpointLsn,
																	 const MetadataPublishOptions &options);
		[[nodiscard]] EntityPageBatchResult applyBatchTransactionInternal(
				const MetadataSnapshotPin &snapshot, std::span<const EntityPageBatchMutation> mutations,
				std::span<const EntityPageBatchAppend> appends, std::span<const EntityPageBatchNewPool> newPools,
				std::span<const EntityPropertyRootMutationPlan> propertyPlans,
				const PropertyCollectionRootStore *propertyRoots,
				const PropertyCollectionRootStoreOptions *propertyOptions,
				const EntityPageAuxiliaryMutationBatch *auxiliaryPages,
				const NativeGraphDerivedStateMutation *derivedState,
				const NativeGraphDerivedStatePreparer *derivedStatePreparer,
				const NativeGraphVectorBootstrapPreparer *vectorBootstrapPreparer,
				const NativeGraphVectorDeltaPreparer *vectorDeltaPreparer, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &options, NativeGraphLogicalIdReservation logicalIdReservation,
				const NativeGraphPrivateSegmentImagePreparer *privateSegmentImagePreparer = nullptr,
				const NativeGraphPrivateSegmentPreparer *privateSegmentPreparer = nullptr,
				const NativeGraphDeferredIndexPlanner *deferredIndexPlanner = nullptr,
                const NativeGraphEarlyPublicationPlanner *earlyPublicationPlanner = nullptr,
                const NativeHybridTopologyPublication *hybridTopology = nullptr);

		static void validateNewPoolRequest(const EntityPageNewPoolRequest &request, uint64_t entityId,
										   EntityPageRecordKind kind, LogSequenceNumber checkpointLsn);
		static void validateAppendRequest(const EntityPageAppendRequest &request, uint64_t entityId,
										  EntityPageRecordKind kind, LogSequenceNumber checkpointLsn);
		// Public entry points select one record type statically. Persisted-record
		// validation therefore stays type-specific instead of accepting nullable,
		// runtime-tagged arguments at this internal boundary.
		static void validateRecord(const NodeEntityRecord &node);
		static void validateRecord(const EdgeEntityRecord &edge);
		static PageOwnerHeader ownerFor(PageId pageId, uint64_t ownerId, Generation generation,
										LogSequenceNumber checkpointLsn);
		static EntityPageLocation mappingFor(uint64_t entityId, PageId pageId, const EntityPage &page, uint16_t slot);
		// Metadata validates the Catalog/Page Directory fences before these
		// callbacks run; SharedPagePoolAllocator validates the resulting frame
		// images before data can become reachable.
		void prepareNewPoolPage(const CatalogEntry &pool, const EntityPage &page, LogSequenceNumber checkpointLsn,
								PageLocation &location) const;
		void prepareAppendPage(const CatalogEntry &pool, const EntityPage &page, LogSequenceNumber checkpointLsn,
							   PageLocation &location) const;
		void prepareReplacementPage(const CatalogEntry &pool, const PageLocation &expected,
									LogSequenceNumber expectedOwnerLsn, const EntityPage &page,
									LogSequenceNumber checkpointLsn, PageLocation &replacement) const;

		SharedPagePoolAllocator allocator_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
