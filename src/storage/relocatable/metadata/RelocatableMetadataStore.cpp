/**
 * @file RelocatableMetadataStore.cpp
 * @brief Serialized operations for the experimental relocatable Catalog metadata stack.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/OwnedVector.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

        using LogicalIdCapabilityMap = std::pmr::map<uint64_t, uint64_t>;
        struct PreparedLogicalIdCapabilities {
            LogicalIdCapabilityMap nodes;
            LogicalIdCapabilityMap edges;
            explicit PreparedLogicalIdCapabilities(std::pmr::memory_resource *memory) : nodes(memory), edges(memory) {}
        };
        // Prepare both kinds atomically. No allocating map insertion may follow
        // durable namespace publication or expose only half a returned lease.
        PreparedLogicalIdCapabilities prepareLogicalIdCapabilities(
                std::span<const NativeGraphLogicalIdLeaseRange> nodes,
                std::span<const NativeGraphLogicalIdLeaseRange> edges,
                const LogicalIdCapabilityMap &outstandingNodes,
                const LogicalIdCapabilityMap &outstandingEdges, const char *message) {
            PreparedLogicalIdCapabilities prepared(outstandingNodes.get_allocator().resource());
            const auto overlaps = [](const LogicalIdCapabilityMap &entries, uint64_t first, uint64_t last) {
                const auto next = entries.lower_bound(first);
                if (next != entries.end() && next->first <= last) return true;
                if (next == entries.begin()) return false;
                const auto previous = std::prev(next);
                return previous->second != 0U && previous->first + previous->second - 1U >= first;
            };
            const auto prepare = [&](std::span<const NativeGraphLogicalIdLeaseRange> ranges,
                                     const LogicalIdCapabilityMap &outstanding, LogicalIdCapabilityMap &target) {
                for (const auto &range : ranges) {
                    if (range.first_id == 0U || range.count == 0U ||
                        range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id)
                        throw FormatException(FormatError::FERR_INVALID_STATE, message);
                    const auto last = range.first_id + range.count - 1U;
                    if (overlaps(outstanding, range.first_id, last) || overlaps(target, range.first_id, last))
                        throw FormatException(FormatError::FERR_INVALID_STATE, message);
                    target.emplace(range.first_id, range.count);
                }
            };
            prepare(nodes, outstandingNodes, prepared.nodes);
            prepare(edges, outstandingEdges, prepared.edges);
            return prepared;
        }

		// This primitive only retires Page Directory routes. Its callers support the
		// four derived page-pool families whose owners need no other directory update.
		[[nodiscard]] bool matchesPageDirectoryOnlyPoolOwnerKind(SegmentKind poolKind, PageOwnerKind ownerKind) {
			switch (poolKind) {
				case SegmentKind::SGK_INDEX_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_INDEX;
				case SegmentKind::SGK_VECTOR_RAW_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_RAW;
				case SegmentKind::SGK_VECTOR_PQ_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_PQ;
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
				default: // validated B+Tree and Vector reclaimers construct retirement plans only for the four supported pool families above.
					return false;
			}
		}

        MetadataPagePoolAllocator openMetadataPoolWithControlScratch(
                const std::shared_ptr<StorageIO> &io, const Superblock &current, bool readOnly,
                const std::shared_ptr<std::pmr::memory_resource> &controlOwner) {
            std::optional<utils::OwnedAllocationScope> scratch;
            if (controlOwner) scratch.emplace(controlOwner);
            // Guaranteed copy elision constructs the nonmovable allocator in
            // its final member; constructor recovery/discovery uses MAINT.
            return MetadataPagePoolAllocator(io, current, readOnly
                ? MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY
                : MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
        }

	} // namespace

	RelocatableMetadataStore::RelocatableMetadataStore(std::shared_ptr<StorageIO> io, const Superblock &current,
													   bool readOnly, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) :
		io_(std::move(io)), bootstrap_(io_), pages_(io_, current),
		metadata_page_pool_(openMetadataPoolWithControlScratch(io_, current, readOnly, controlOwner)),
		catalog_(pages_, metadata_page_pool_), locations_(pages_, metadata_page_pool_),
		allocator_(pages_, metadata_page_pool_), pool_ownership_(catalog_, allocator_, metadata_page_pool_, readOnly),
		snapshots_(bootstrap_, {}, std::move(memoryOwner), controlOwner), retirement_(catalog_, locations_, snapshots_),
		reservation_coordinator_(catalog_, allocator_, locations_),
		segment_retirement_coordinator_(catalog_, allocator_, locations_, retirement_),
		segment_relocation_coordinator_(catalog_, allocator_, locations_, retirement_),
		retire_queue_reclaimer_(catalog_, allocator_, locations_, retirement_),
        lease_memory_owner_(std::move(leaseMemoryOwner)),
        outstanding_node_id_leases_(lease_memory_owner_ ? lease_memory_owner_.get() : std::pmr::new_delete_resource()),
        outstanding_edge_id_leases_(lease_memory_owner_ ? lease_memory_owner_.get() : std::pmr::new_delete_resource()),
        read_only_(readOnly) {
        std::optional<utils::OwnedAllocationScope> scratch;
        if (controlOwner) scratch.emplace(controlOwner);
		pool_ownership_.restoreCatalogOwnership(current);
		pool_ownership_.verifyAfterOpen(current);
	}

	RelocatableMetadataStore::~RelocatableMetadataStore() = default;

	MetadataWriteGuard RelocatableMetadataStore::beginWrite() {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot publish or reclaim metadata");
		}
		return snapshots_.beginWrite();
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::initializeNew(std::shared_ptr<StorageIO> io,
																					  const Superblock &initial, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		initializeRelocatableMetadataFile(io, initial);
		return open(std::move(io), std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::open(std::shared_ptr<StorageIO> io, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, false, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner)));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::openReadOnly(std::shared_ptr<StorageIO> io, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, true, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner)));
	}

	MetadataSnapshotPin RelocatableMetadataStore::openSnapshot() const { return snapshots_.openSnapshot(); }

	void RelocatableMetadataStore::refreshAfterExternalPublication() {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot refresh a writable publication state");
		}
		MetadataWriteGuard writer = beginWrite();
		if (snapshots_.activeSnapshotCount() != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata refresh requires all local snapshots to be released");
		}
		const Superblock current = bootstrap_.recover();
		metadata_page_pool_.rebaseToPublishedSuperblock(current);
		pool_ownership_.restoreCatalogOwnership(current);
		metadata_page_pool_.recover(current);
		pool_ownership_.verifyAfterOpen(current);
	}

	NativeGraphLogicalIdLease
	RelocatableMetadataStore::reserveNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount,
														   const MetadataPublishOptions &options) {
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph logical-ID leasing requires a writable store and positive bounded work");
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		// This direct, failpoint-aware API cannot silently consume a cached range:
		// discard its unused tail so every direct call begins at the durable
		// high-water mark and never lets an older cached ID overtake it later.
		available_node_ids_ = {};
		available_edge_ids_ = {};
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (!current.checkpoint_lsn.isValid() ||
			current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max() ||
			(current.next_logical_node_id == 0U && nodeCount != 0U) ||
			(current.next_logical_edge_id == 0U && edgeCount != 0U) ||
			(nodeCount != 0U && nodeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_node_id) ||
			(edgeCount != 0U && edgeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_edge_id)) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID lease exceeds a durable logical-ID namespace");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		NativeGraphLogicalIdLease lease;
		if (nodeCount != 0U) {
			lease.nodes = {batch.allocateLogicalNodeIdRange(nodeCount), nodeCount};
		}
		if (edgeCount != 0U) {
			lease.edges = {batch.allocateLogicalEdgeIdRange(edgeCount), edgeCount};
		}
        auto prepared = prepareLogicalIdCapabilities(
            nodeCount == 0U ? std::span<const NativeGraphLogicalIdLeaseRange>{} : std::span(&lease.nodes, 1U),
            edgeCount == 0U ? std::span<const NativeGraphLogicalIdLeaseRange>{} : std::span(&lease.edges, 1U),
            outstanding_node_id_leases_, outstanding_edge_id_leases_,
            "native graph logical-ID lease capability overlaps an outstanding lease");
		// Leasing changes only durable high-water marks; it must not create a
		// metadata-pool ownership transition or perturb graph-visible roots. A
		// bootstrap header still advances so the candidate generation retains a
		// same-generation route context for the next graph publication.
		prepareBootstrapRoutes(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
        // The lease mutex excludes intervening registrations; these maps use
        // equal std allocators and transfer their prepared nodes without allocation.
        outstanding_node_id_leases_.merge(prepared.nodes);
        outstanding_edge_id_leases_.merge(prepared.edges);
		lease.superblock = batch.candidate();
		return lease;
	}

	NativeGraphLogicalIdReservation
	RelocatableMetadataStore::acquireNativeGraphLogicalIds(uint64_t nodeCount, uint64_t edgeCount,
														   uint64_t minimumLeaseBlockSize) {
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U) || minimumLeaseBlockSize == 0U ||
			minimumLeaseBlockSize == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph logical-ID acquisition requires a writable store and bounded positive work");
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		const auto validateRequest = [](uint64_t count, uint64_t first, const char *entityName) {
			if (count != 0U && (first == 0U || count - 1U > std::numeric_limits<uint64_t>::max() - first)) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  std::string("native graph ") + entityName +
											  " logical-ID acquisition exceeds its durable namespace");
			}
		};
		const auto take = [](NativeGraphLogicalIdLeaseRange &available, uint64_t &remaining,
							 std::vector<NativeGraphLogicalIdLeaseRange> &result) {
			if (remaining == 0U || available.empty()) {
				return;
			}
			const uint64_t taken = std::min(available.count, remaining);
			result.push_back({available.first_id, taken});
			available.first_id += taken;
			available.count -= taken;
			remaining -= taken;
		};
		uint64_t remainingNodes = nodeCount;
		uint64_t remainingEdges = edgeCount;
		NativeGraphLogicalIdReservation result;
		take(available_node_ids_, remainingNodes, result.node_ranges);
		take(available_edge_ids_, remainingEdges, result.edge_ranges);
        const auto prepareOutstanding = [this](const NativeGraphLogicalIdReservation &reservation) {
            return prepareLogicalIdCapabilities(reservation.node_ranges, reservation.edge_ranges,
                outstanding_node_id_leases_, outstanding_edge_id_leases_,
                "native graph logical-ID acquisition overlaps an outstanding lease");
        };
        const auto registerPrepared = [this](PreparedLogicalIdCapabilities &prepared) {
            outstanding_node_id_leases_.merge(prepared.nodes);
            outstanding_edge_id_leases_.merge(prepared.edges);
        };
		if (remainingNodes == 0U && remainingEdges == 0U) {
            auto prepared = prepareOutstanding(result);
            registerPrepared(prepared);
			return result;
		}
		const uint64_t requestedNodes = remainingNodes == 0U ? 0U : std::max(remainingNodes, minimumLeaseBlockSize);
		const uint64_t requestedEdges = remainingEdges == 0U ? 0U : std::max(remainingEdges, minimumLeaseBlockSize);
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		validateRequest(requestedNodes, current.next_logical_node_id, "Node");
		validateRequest(requestedEdges, current.next_logical_edge_id, "Edge");
		if (!current.checkpoint_lsn.isValid() ||
			current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID acquisition cannot advance the checkpoint LSN");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		if (requestedNodes != 0U) {
			static_cast<void>(batch.allocateLogicalNodeIdRange(requestedNodes));
		}
		if (requestedEdges != 0U) {
			static_cast<void>(batch.allocateLogicalEdgeIdRange(requestedEdges));
		}
        NativeGraphLogicalIdLeaseRange nextAvailableNodes{current.next_logical_node_id, requestedNodes};
        NativeGraphLogicalIdLeaseRange nextAvailableEdges{current.next_logical_edge_id, requestedEdges};
        if (remainingNodes != 0U) take(nextAvailableNodes, remainingNodes, result.node_ranges);
        if (remainingEdges != 0U) take(nextAvailableEdges, remainingEdges, result.edge_ranges);
        auto prepared = prepareOutstanding(result);
        prepareBootstrapRoutes(batch);
        batch.publish(bootstrap_);
        snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
        if (requestedNodes != 0U) available_node_ids_ = nextAvailableNodes;
        if (requestedEdges != 0U) available_edge_ids_ = nextAvailableEdges;
        registerPrepared(prepared);
		return result;
	}

	bool
	RelocatableMetadataStore::claimNativeGraphLogicalIdReservation(const NativeGraphLogicalIdReservation &reservation) {
		if (reservation.empty()) {
			return true;
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		const auto validateRanges = [](const std::pmr::map<uint64_t, uint64_t> &outstanding,
									   std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
			std::set<uint64_t> firstIds;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				const auto iterator = outstanding.find(range.first_id);
				if (range.first_id == 0U || range.count == 0U || !firstIds.insert(range.first_id).second ||
					iterator == outstanding.end() || iterator->second != range.count) {
					return false;
				}
			}
			return true;
		};
		if (!validateRanges(outstanding_node_id_leases_, reservation.node_ranges) ||
			!validateRanges(outstanding_edge_id_leases_, reservation.edge_ranges)) {
			return false;
		}
		for (const NativeGraphLogicalIdLeaseRange &range: reservation.node_ranges) {
			outstanding_node_id_leases_.erase(range.first_id);
		}
		for (const NativeGraphLogicalIdLeaseRange &range: reservation.edge_ranges) {
			outstanding_edge_id_leases_.erase(range.first_id);
		}
		return true;
	}

	void RelocatableMetadataStore::abandonNativeGraphLogicalIdReservation(
			const NativeGraphLogicalIdReservation &reservation) noexcept {
        abandonNativeGraphLogicalIdReservation(reservation.node_ranges, reservation.edge_ranges);
    }

    void RelocatableMetadataStore::abandonNativeGraphLogicalIdReservation(
            std::span<const NativeGraphLogicalIdLeaseRange> nodes,
            std::span<const NativeGraphLogicalIdLeaseRange> edges) noexcept {
		try {
			std::lock_guard leaseLock(logical_id_lease_mutex_);
			const auto abandonRanges = [](std::pmr::map<uint64_t, uint64_t> &outstanding,
										  std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
					const auto iterator = outstanding.find(range.first_id);
					if (iterator != outstanding.end() && iterator->second == range.count) {
						outstanding.erase(iterator);
					}
				}
			};
			abandonRanges(outstanding_node_id_leases_, nodes);
			abandonRanges(outstanding_edge_id_leases_, edges);
		} catch (...) {
			// Rollback is best-effort only for an in-memory capability. Reuse still
			// cannot occur after process restart because durable high-water marks only advance.
		}
	}

	bool RelocatableMetadataStore::ownsSnapshot(const MetadataSnapshotPin &snapshot) const noexcept {
		return snapshots_.owns(snapshot);
	}

	bool RelocatableMetadataStore::isIdentityOnlyBootstrapAdvance(const Superblock &before,
																  const Superblock &after) const {
		if (!before.usesPageDirectoryBootstrap() || !after.usesPageDirectoryBootstrap() ||
			!before.page_directory_root.isValid() || !after.page_directory_root.isValid()) {
			return false;
		}
		const PageDirectoryBootstrap previous = pages_.readPageDirectoryBootstrap(before.page_directory_root);
		const PageDirectoryBootstrap current = pages_.readPageDirectoryBootstrap(after.page_directory_root);
		return previous.data_page_directory_root == current.data_page_directory_root &&
			   previous.metadata_page_directory_root == current.metadata_page_directory_root &&
			   previous.metadata_directory_roots == current.metadata_directory_roots;
	}

	void RelocatableMetadataStore::prepareBootstrapRoutes(MetadataWriteBatch &batch) {
		if (batch.candidate().usesPageDirectoryBootstrap() && !batch.bootstrapRoutePublished()) {
			MetadataPageRouteDirectory routes(pages_);
			if (batch.metadataPageRoutes().empty() && !batch.pendingBootstrapDataRoot().has_value() &&
				!batch.bootstrapRouteRefreshRequested()) {
				batch.reserveMetadataFrames(1U);
				static_cast<void>(routes.publishIdentityOnlyBootstrap(batch));
				return;
			}
			const size_t requiredFrames = routes.nextBootstrapFrameCount(batch);
			batch.reserveMetadataFrames(requiredFrames);
			static_cast<void>(
					routes.publishNextBootstrap(batch, batch.pendingBootstrapDataRoot().value_or(PhysicalPageRef{})));
		}
	}

	void RelocatableMetadataStore::publishBatch(MetadataWriteBatch &batch, const MetadataWriteGuard &writer,
												const MetadataPublishOptions &options) {
		// The first Catalog COW can allocate a metadata pool that was not visible
		// while the ownership pass enumerated pools. Reconcile it while the Catalog
		// root still uses physical children; logical route children are not visible
		// until the final bootstrap publication.
		pool_ownership_.preparePublication(batch);
		catalog_.prepareLogicalRootChildren(batch);
		// Catalog logicalization can allocate a metadata pool. Reconcile it while
		// the allocator root is still physical so setting its ownership bit composes
		// with all data-pool allocations already staged in this batch.
		pool_ownership_.preparePublication(batch);
		allocator_.prepareLogicalRootChildren(batch);
		locations_.prepareLogicalRootChildren(batch);
		// Any metadata pool created by allocator or location-root logicalization is
		// reconciled through the pending route overlay before the final publication.
		pool_ownership_.preparePublication(batch);
		if (batch.candidate().usesPageDirectoryBootstrap()) {
			MetadataPageRouteDirectory routes(pages_);
			for (size_t pass = 0U; pass < 8U; ++pass) {
				const bool identityOnly = batch.metadataPageRoutes().empty() &&
										  !batch.pendingBootstrapDataRoot().has_value() &&
										  !batch.bootstrapRouteRefreshRequested();
				const size_t frameCount = identityOnly ? 1U : routes.nextBootstrapFrameCount(batch);
				if (batch.ensureMetadataFrameCapacity(frameCount) == 0U) {
					break;
				}
				// A capacity pool is private until this Catalog publication. Logicalize
				// the resulting COW root before calculating the final route budget again.
				pool_ownership_.preparePublication(batch);
				catalog_.prepareLogicalRootChildren(batch);
				allocator_.prepareLogicalRootChildren(batch);
				locations_.prepareLogicalRootChildren(batch);
				if (pass == 7U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable metadata route-capacity publication did not converge");
				}
			}
		}
		prepareBootstrapRoutes(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		pages_.rebindSuperblock(batch.candidate());
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
	}

	Superblock RelocatableMetadataStore::upsertCatalog(std::span<const CatalogEntry> entries,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const CatalogEntry &entry: entries) {
			batch.reserveDataSlot(entry.physical_slot);
			batch.observeLogicalSegmentId(entry.segment_id);
		}
		catalog_.prepareUpsert(batch, entries);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertLocations(std::span<const PageLocation> pages,
														 std::span<const ExtentLocation> extents,
														 LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options) {
		return upsertLocationsImpl(pages, extents, checkpointLsn, nullptr, options);
	}

	Superblock RelocatableMetadataStore::upsertLocations(std::span<const PageLocation> pages,
														 std::span<const ExtentLocation> extents,
														 LogSequenceNumber checkpointLsn,
														 const LocationPublicationPreparer &preparer,
														 const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable prepared location publication requires a data preparation callback");
		}
		return upsertLocationsImpl(pages, extents, checkpointLsn, &preparer, options);
	}

	Superblock RelocatableMetadataStore::upsertEntityPageLocations(std::span<const EntityPageLocation> nodes,
																   std::span<const uint64_t> erasedNodeIds,
																   std::span<const EntityPageLocation> edges,
																   std::span<const uint64_t> erasedEdgeIds,
																   LogSequenceNumber checkpointLsn,
																   const MetadataPublishOptions &options) {
		if ((nodes.empty() && erasedNodeIds.empty()) && (edges.empty() && erasedEdgeIds.empty())) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Map publication requires a Node or Edge mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const auto validateLocation = [&](const EntityPageLocation &location) {
			const std::optional<PageLocation> page = locations_.findPageOptional(current, location.page_id);
			if (!page.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Map entry references an absent Page Directory entry");
			}
			const CatalogEntry pool = catalog_.find(current, page->segment_id);
			const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page->generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Map entry does not resolve to a current entity page pool");
			}
		};
		for (const EntityPageLocation &location: nodes) {
			validateLocation(location);
		}
		for (const EntityPageLocation &location: edges) {
			validateLocation(location);
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		if (!nodes.empty() || !erasedNodeIds.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, erasedNodeIds);
		}
		if (!edges.empty() || !erasedEdgeIds.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, edges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertEntityPropertyLocations(
			std::span<const EntityPropertyLocation> locations, std::span<const EntityPropertyKey> erasedKeys,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		if (locations.empty() && erasedKeys.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Property location publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const EntityPropertyLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(!location.key.isValid()) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity Property location does not target a current Property page");
			}
		}
		for (const EntityPropertyKey &key: erasedKeys) {
			if (!key.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Entity Property deletion has an invalid key");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceEntityProperties(batch, locations, erasedKeys);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<EntityPropertyPagePoolMaterialization> RelocatableMetadataStore::materializeEntityPropertyPagePool(
			uint64_t pageOwnerId, EntityPropertyKey property, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const EntityPropertyPagePoolPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput =
				static_cast<uint8_t>(pageOwnerId == 0U) | static_cast<uint8_t>(!property.isValid()) |
				static_cast<uint8_t>(!firstSlot.isValid()) | static_cast<uint8_t>(!lastSlot.isValid()) |
				static_cast<uint8_t>(firstSlot > lastSlot) | static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity Property page materialization requires an owner, key, slot "
								  "range, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (locations_.findEntityPropertyOptional(current, property).has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page materialization cannot replace an existing owner key");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId segmentId = batch.allocateLogicalSegmentId();
		const PageId pageId = batch.allocateLogicalPageId();
		// The writer-held MetadataWriteBatch allocates from durable high-water
		// marks, so newly issued Segment and Page IDs cannot already be present.
		if (catalog_.findOptional(current, segmentId).has_value() ||
			locations_.findPageOptional(current, pageId).has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page materialization allocated a published logical identity");
		}
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}

		CatalogEntry pool;
		pool.segment_id = segmentId;
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		pool.owner_id = pageOwnerId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		batch.observeLogicalSegmentId(segmentId);

		PageLocation page{pageId, segmentId, 0U, pool.generation, 0U};
		EntityPropertyLocation location{property, pageId, 0U, 0U};
		preparer(pool, pageId, page, location);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id != pageId) |
										static_cast<uint8_t>(page.segment_id != pool.segment_id) |
										static_cast<uint8_t>(page.generation != pool.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(page.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(location.key != property) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Property page preparer returned an invalid page or directory location");
		}

		catalog_.prepareUpsert(batch, std::span(&pool, 1));
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		locations_.prepareReplaceEntityProperties(batch, std::span(&location, 1), {});
		publishBatch(batch, writer, options);
		return EntityPropertyPagePoolMaterialization{batch.candidate(), pool, page, location};
	}

	std::optional<StatePagePoolMaterialization>
	RelocatableMetadataStore::materializeStatePagePool(uint64_t pageOwnerId, uint64_t stateId, PhysicalSlotId firstSlot,
													   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
													   const StatePagePoolPreparer &preparer,
													   const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pageOwnerId == 0U) | static_cast<uint8_t>(stateId == 0U) |
									 static_cast<uint8_t>(!firstSlot.isValid()) |
									 static_cast<uint8_t>(!lastSlot.isValid()) |
									 static_cast<uint8_t>(firstSlot > lastSlot) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable State page materialization requires an owner, State ID, slot range, "
								  "LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (locations_.findStateOptional(current, stateId).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page materialization cannot replace an existing State ID");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId segmentId = batch.allocateLogicalSegmentId();
		const PageId pageId = batch.allocateLogicalPageId();
		// The writer-held MetadataWriteBatch allocates from durable high-water
		// marks, so newly issued Segment and Page IDs cannot already be present.
		if (catalog_.findOptional(current, segmentId).has_value() ||
			locations_.findPageOptional(current, pageId).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page materialization allocated a published logical identity");
		}
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}

		CatalogEntry pool;
		pool.segment_id = segmentId;
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_STATE_PAGE_POOL;
		pool.owner_id = pageOwnerId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		batch.observeLogicalSegmentId(segmentId);

		PageLocation page{pageId, segmentId, 0U, pool.generation, 0U};
		StateLocation state{stateId, pageId, 0U, 0U};
		preparer(pool, pageId, page, state);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id != pageId) |
										static_cast<uint8_t>(page.segment_id != pool.segment_id) |
										static_cast<uint8_t>(page.generation != pool.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(page.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(state.state_id != stateId) |
										static_cast<uint8_t>(state.page_id != pageId) |
										static_cast<uint8_t>(state.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(state.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page preparer returned an invalid page or directory location");
		}

		catalog_.prepareUpsert(batch, std::span(&pool, 1));
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		locations_.prepareReplaceStates(batch, std::span(&state, 1), {});
		publishBatch(batch, writer, options);
		return StatePagePoolMaterialization{batch.candidate(), pool, page, state};
	}

	Superblock RelocatableMetadataStore::replaceStatePageAndUpsertLocation(const PageLocation &expectedPage,
																		   StateLocation state,
																		   LogSequenceNumber checkpointLsn,
																		   const StatePageReplacementPreparer &preparer,
																		   const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedPage.page_id.isValid()) |
									 static_cast<uint8_t>(!expectedPage.segment_id.isValid()) |
									 static_cast<uint8_t>(!expectedPage.generation.isValid()) |
									 static_cast<uint8_t>(expectedPage.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									 static_cast<uint8_t>(expectedPage.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									 static_cast<uint8_t>(state.state_id == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable State page replacement requires a current page, State ID, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const PageLocation currentPage = locations_.findPage(current, expectedPage.page_id);
		const CatalogEntry pool = catalog_.find(current, currentPage.segment_id);
		const std::optional<StateLocation> visibleState = locations_.findStateOptional(current, state.state_id);
		const uint8_t invalidCurrent =
				static_cast<uint8_t>(currentPage != expectedPage) |
				static_cast<uint8_t>(pool.kind != SegmentKind::SGK_STATE_PAGE_POOL) |
				static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(pool.generation != currentPage.generation) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot)) |
				static_cast<uint8_t>(visibleState.has_value() && *visibleState != state);
		if (invalidCurrent != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State page replacement has a stale page, non-State pool, or State route");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		PageLocation replacement = expectedPage;
		preparer(pool, expectedPage, replacement, state);
		const uint8_t invalidPrepared = static_cast<uint8_t>(replacement.page_id != expectedPage.page_id) |
										static_cast<uint8_t>(replacement.segment_id != pool.segment_id) |
										static_cast<uint8_t>(replacement.generation != pool.generation) |
										static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(replacement.free_bytes > RSS_PAGE_SIZE) |
										static_cast<uint8_t>(state.page_id != expectedPage.page_id) |
										static_cast<uint8_t>(state.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(state.slot_generation == 0U);
		if (invalidPrepared != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State page replacement preparer returned an invalid page or directory location");
		}

		locations_.prepareReplacePages(batch, std::span(&replacement, 1), {});
		locations_.prepareReplaceStates(batch, std::span(&state, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertStateLocations(std::span<const StateLocation> locations,
															  std::span<const uint64_t> erasedStateIds,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options) {
		if (locations.empty() && erasedStateIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable State location publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const StateLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(location.state_id == 0U) |
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
									static_cast<uint8_t>(location.slot_generation == 0U) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_STATE_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable State location does not target a current State page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceStates(batch, locations, erasedStateIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertVectorLocations(std::span<const VectorLocation> locations,
															   std::span<const uint64_t> erasedVectorIds,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (locations.empty() && erasedVectorIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Location Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const VectorLocation &location: locations) {
			const PageLocation page = locations_.findPage(current, location.page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const bool vectorPool =
					pool.kind == SegmentKind::SGK_VECTOR_RAW_POOL || pool.kind == SegmentKind::SGK_VECTOR_PQ_POOL;
			const uint8_t invalid =
					static_cast<uint8_t>(location.vector_id == 0U) | static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
					static_cast<uint8_t>(location.slot_generation == 0U) | static_cast<uint8_t>(!vectorPool) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(pool.generation != page.generation) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector location does not target a current raw or PQ vector page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceVectorLocations(batch, locations, erasedVectorIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertVectorManifests(std::span<const VectorManifestLocation> manifests,
															   std::span<const uint64_t> erasedIndexIds,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (manifests.empty() && erasedIndexIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Manifest Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const VectorManifestLocation &manifest: manifests) {
			const PageLocation page = locations_.findPage(current, manifest.manifest_page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(manifest.index_id == 0U) |
									static_cast<uint8_t>(!manifest.manifest_page_id.isValid()) |
									static_cast<uint8_t>(!manifest.generation.isValid()) |
									static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
									static_cast<uint8_t>(pool.owner_id != manifest.index_id) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(page.generation != manifest.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector manifest does not target a current dedicated manifest page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceVectorManifests(batch, manifests, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::upsertIndexRoots(std::span<const IndexRootLocation> roots,
														  std::span<const uint64_t> erasedIndexIds,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options) {
		if (roots.empty() && erasedIndexIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Index Root Directory publication requires a mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		for (const IndexRootLocation &root: roots) {
			const uint8_t invalidIdentity = static_cast<uint8_t>(root.index_id == 0U) |
											static_cast<uint8_t>(!root.root_page_id.isValid()) |
											static_cast<uint8_t>(!root.root_generation.isValid());
			if (invalidIdentity != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates every typed root before
				// invoking this private metadata-only directory publication primitive.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Index Root Directory entry has an invalid logical identity");
			}
			const PageLocation page = locations_.findPage(current, root.root_page_id);
			const CatalogEntry pool = catalog_.find(current, page.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(pool.generation != page.generation) |
									static_cast<uint8_t>(page.generation != root.root_generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Index Root Directory entry does not target a current index page");
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareReplaceIndexRoots(batch, roots, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewIndexTreePages(SegmentId poolSegmentId, uint64_t indexId,
																	  size_t pageCount, size_t rootPageOrdinal,
																	  LogSequenceNumber checkpointLsn,
																	  const NewIndexTreePagesPreparer &preparer,
																	  const MetadataPublishOptions &options) {
		if (!poolSegmentId.isValid() || indexId == 0U || pageCount == 0U ||
			pageCount > RSS_PAGE_POOL_DATA_FRAME_COUNT || rootPageOrdinal >= pageCount ||
			!preparer) { // ZYX_COV_EXCL_LINE:
			// only BTreePageStore reaches this private boundary after input validation.
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree materialization requires a bounded page batch, root ordinal, and preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry pool = catalog_.find(current, poolSegmentId);
		const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
		if (invalidPool != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates the requested pool before this
			// private metadata boundary; retain the check for a relocation race while the writer is acquired.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree materialization requires a current allocated index page pool");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<PageId> pageIds;
		std::vector<PageLocation> locations;
		pageIds.reserve(pageCount);
		locations.reserve(pageCount);
		for (size_t index = 0; index < pageCount; ++index) {
			const PageId pageId = batch.allocateLogicalPageId();
			if (locations_.findPageOptional(current, pageId).has_value()) { // ZYX_COV_EXCL_LINE: every Page
				// Directory publication advances the durable high-water mark before a later allocation can reuse this
				// ID.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable logical Page ID high-water mark collides with an existing Page Directory entry");
			}
			pageIds.push_back(pageId);
			locations.push_back({pageId, pool.segment_id, 0, pool.generation, 0});
		}
		preparer(pool, pageIds, locations);

		std::set<PageId> uniquePageIds;
		std::set<uint16_t> uniqueFrames;
		for (size_t index = 0; index < locations.size(); ++index) {
			const PageLocation &location = locations[index];
			const uint8_t invalidLocation = static_cast<uint8_t>(location.page_id != pageIds[index]) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(location.generation != pool.generation) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
			if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
				!uniqueFrames.insert(location.page_index).second) { // ZYX_COV_EXCL_LINE: the private typed preparer
				// derives one location and one unique frame from every allocated logical Page ID.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable B+Tree materialization returned invalid or duplicate Page Directory locations");
			}
		}
		const IndexRootLocation root{indexId, pageIds[rootPageOrdinal], pool.generation};
		locations_.prepareUpsertPages(batch, locations);
		locations_.prepareReplaceIndexRoots(batch, std::span(&root, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewIndexTreePagesAcrossPools(
			std::span<const NewIndexTreeMaterializationPoolRequest> pools, uint64_t indexId,
			IndexTreeMaterializationPageReference rootPage, LogSequenceNumber checkpointLsn,
			const IndexTreeMultiPoolMaterializationPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer) |
									 static_cast<uint8_t>(rootPage.pool_ordinal >= pools.size());
		if (invalidInput != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates the public build plan first.
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable bulk B+Tree materialization requires pools, an index, a root, an LSN, and a preparer");
		}

		size_t totalPageCount = 0;
		for (const NewIndexTreeMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPageCount) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable bulk B+Tree materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}
		if (rootPage.page_ordinal >= pools[rootPage.pool_ordinal].page_count) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable bulk B+Tree materialization root does not name one requested page");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);

		std::vector<IndexTreeMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const NewIndexTreeMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable bulk B+Tree materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
			pool.owner_id = request.owner_id;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			IndexTreeMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) { // ZYX_COV_EXCL_LINE: the durable
					// high-water mark makes a freshly allocated Page ID distinct from every current mapping.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable bulk B+Tree materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedLocations;
		publishedLocations.reserve(totalPageCount);
		for (const IndexTreeMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() != pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: BTreePageStore preserves each
				// metadata-assigned batch cardinality while writing its corresponding physical frames.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable bulk B+Tree materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame).second) { // ZYX_COV_EXCL_LINE: BTreePageStore derives unique Page IDs
					// and physical frames directly from one newly reserved pool batch.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable bulk B+Tree materialization returned invalid or duplicate page locations");
				}
				publishedLocations.push_back(location);
			}
		}

		const IndexTreeMaterializationPoolBatch &rootPool = batches[rootPage.pool_ordinal];
		const IndexRootLocation root{indexId, rootPool.page_ids[rootPage.page_ordinal], rootPool.pool.generation};
		std::ranges::sort(publishedLocations, {}, &PageLocation::page_id);
		locations_.prepareUpsertPages(batch, publishedLocations);
		locations_.prepareReplaceIndexRoots(batch, std::span(&root, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}


	Superblock RelocatableMetadataStore::mutateIndexTreePagesAcrossPools(
			std::span<const IndexTreeMutationPoolRequest> requests,
			std::optional<NewIndexTreePoolRequest> newPoolRequest, const IndexRootLocation &expectedRoot,
			std::optional<IndexTreeMutationAppendedPageReference> replacementRoot,
			std::optional<PageId> stableReplacementRoot, LogSequenceNumber checkpointLsn,
			const IndexTreeMultiPoolMutationPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(requests.empty()) |
									 static_cast<uint8_t>(expectedRoot.index_id == 0U) |
									 static_cast<uint8_t>(!expectedRoot.root_page_id.isValid()) |
									 static_cast<uint8_t>(!expectedRoot.root_generation.isValid()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		// ZYX_COV_EXCL_START: BTreePageStore is the sole caller and validates this immutable plan input.
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable multi-pool B+Tree mutation requires pools, a root, an LSN, and a preparer");
		}
		// ZYX_COV_EXCL_STOP

		size_t totalAppended = 0;
		std::set<SegmentId> requestedPools;
		std::set<PageId> replacementIds;
		std::set<PageId> retirementIds;
		for (const IndexTreeMutationPoolRequest &request: requests) {
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.expected_replacements.empty() && request.appended_page_count == 0U &&
											 request.expected_retirements.empty()) |
					static_cast<uint8_t>(request.replacement_segment_id.isValid() &&
											 request.expected_replacements.empty());
			// ZYX_COV_EXCL_START: the B+Tree planner groups each validated pool exactly once.
			if (invalidRequest != 0U || !requestedPools.insert(request.segment_id).second) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable multi-pool B+Tree mutation has an invalid or duplicate pool request");
			}
			// ZYX_COV_EXCL_STOP
			// ZYX_COV_EXCL_START: B+Tree plans are bounded by the shared-pool frame count.
			if (request.appended_page_count > std::numeric_limits<size_t>::max() - totalAppended) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable multi-pool B+Tree mutation page count overflows host memory");
			}
			// ZYX_COV_EXCL_STOP
			totalAppended += request.appended_page_count;
			for (const PageLocation &expected: request.expected_replacements) {
				// ZYX_COV_EXCL_START: replacement IDs come from one deduplicated pinned mutation path.
				if (!expected.page_id.isValid() || !replacementIds.insert(expected.page_id).second ||
					retirementIds.contains(expected.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"relocatable multi-pool B+Tree mutation has an invalid or duplicate replacement Page ID");
				}
				// ZYX_COV_EXCL_STOP
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				// ZYX_COV_EXCL_START: retirements are emitted only from the same deduplicated mutation path.
				if (!retirement.location.page_id.isValid() || !retirement.expected_owner_lsn.isValid() ||
					!retirementIds.insert(retirement.location.page_id).second ||
					replacementIds.contains(retirement.location.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"relocatable multi-pool B+Tree mutation has an invalid, duplicate, or replaced "
							"retirement Page ID");
				}
				// ZYX_COV_EXCL_STOP
			}
		}
		if (newPoolRequest.has_value()) {
			const uint8_t invalidNewPool =
					static_cast<uint8_t>(!newPoolRequest->first_slot.isValid()) |
					static_cast<uint8_t>(!newPoolRequest->last_slot.isValid()) |
					static_cast<uint8_t>(newPoolRequest->first_slot > newPoolRequest->last_slot) |
					static_cast<uint8_t>(newPoolRequest->appended_page_count == 0U) |
					static_cast<uint8_t>(newPoolRequest->appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			// ZYX_COV_EXCL_START: BTreePageStore constructs growth requests after validation.
			if (invalidNewPool != 0U ||
				newPoolRequest->appended_page_count > std::numeric_limits<size_t>::max() - totalAppended) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable new index pool request requires a valid range and bounded append batch");
			}
			// ZYX_COV_EXCL_STOP
			totalAppended += newPoolRequest->appended_page_count;
		}
		const size_t batchCount = requests.size() + (newPoolRequest.has_value() ? 1U : 0U);
		const bool hasPageMutation = !replacementIds.empty() || !retirementIds.empty() || totalAppended != 0U;
		// ZYX_COV_EXCL_START: root references are derived from this private plan's append ordinals.
		if ((replacementRoot.has_value() && stableReplacementRoot.has_value()) ||
			(stableReplacementRoot.has_value() && !stableReplacementRoot->isValid()) || !hasPageMutation ||
			(replacementRoot.has_value() &&
			 (replacementRoot->pool_ordinal >= batchCount ||
			  (replacementRoot->pool_ordinal < requests.size() &&
			   replacementRoot->appended_page_ordinal >= requests[replacementRoot->pool_ordinal].appended_page_count) ||
			  (replacementRoot->pool_ordinal == requests.size() &&
			   (!newPoolRequest.has_value() ||
				replacementRoot->appended_page_ordinal >= newPoolRequest->appended_page_count))))) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable multi-pool B+Tree mutation has an invalid appended root reference");
		}
		// ZYX_COV_EXCL_STOP

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const std::optional<IndexRootLocation> visibleRoot =
				locations_.findIndexRootOptional(current, expectedRoot.index_id);
		if (!visibleRoot.has_value() || *visibleRoot != expectedRoot) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable multi-pool B+Tree mutation has a stale or unavailable Index root mapping");
		}
		const PageLocation visibleRootLocation = locations_.findPage(current, expectedRoot.root_page_id);
		const CatalogEntry visibleRootPool = catalog_.find(current, visibleRootLocation.segment_id);
		const uint8_t invalidVisibleRoot =
				static_cast<uint8_t>(visibleRootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
				static_cast<uint8_t>(visibleRootPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(visibleRootLocation.generation != expectedRoot.root_generation) |
				static_cast<uint8_t>(visibleRootPool.generation != expectedRoot.root_generation) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visibleRootPool.physical_slot));
		if (invalidVisibleRoot != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable multi-pool B+Tree mutation root does not resolve to one current index pool");
		}

		std::vector<IndexTreeMutationPoolBatch> batches;
		batches.reserve(requests.size());
		for (const IndexTreeMutationPoolRequest &request: requests) {
			const CatalogEntry pool = catalog_.find(current, request.segment_id);
			const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidPool != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable multi-pool B+Tree mutation requires current allocated index page pools");
			}
			CatalogEntry replacementPool = pool;
			if (request.replacement_segment_id.isValid()) {
				replacementPool = catalog_.find(current, request.replacement_segment_id);
				const uint8_t invalidReplacementPool =
						static_cast<uint8_t>(replacementPool.segment_id == pool.segment_id) |
						static_cast<uint8_t>(replacementPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(replacementPool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(replacementPool.owner_id != pool.owner_id) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, replacementPool.physical_slot));
				if (invalidReplacementPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has an unavailable cross-pool replacement destination");
				}
			}
			for (const PageLocation &expected: request.expected_replacements) {
				const PageLocation visible = locations_.findPage(current, expected.page_id);
				const uint8_t invalidReplacement =
						static_cast<uint8_t>(visible != expected) |
						static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
						static_cast<uint8_t>(expected.generation != pool.generation) |
						static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidReplacement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has a stale, foreign, or invalid replacement page");
				}
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
				const PageLocation &expected = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(visible != expected) |
						static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
						static_cast<uint8_t>(expected.generation != pool.generation) |
						static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidRetirement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has a stale, foreign, or invalid retirement page");
				}
			}
			IndexTreeMutationPoolBatch poolBatch;
			poolBatch.pool = pool;
			poolBatch.expected_replacements = request.expected_replacements;
			poolBatch.replacement_locations = request.expected_replacements;
			poolBatch.expected_retirements = request.expected_retirements;
			poolBatch.replacement_pool = replacementPool;
			batches.push_back(std::move(poolBatch));
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (newPoolRequest.has_value()) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, newPoolRequest->first_slot, newPoolRequest->last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable new index pool request has no free physical slot in its reservation range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
			pool.owner_id = newPoolRequest->owner_id;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalog_.prepareUpsert(batch, std::span(&pool, 1));
			IndexTreeMutationPoolBatch poolBatch;
			poolBatch.pool = pool;
			poolBatch.newly_reserved = true;
			poolBatch.replacement_pool = pool;
			batches.push_back(std::move(poolBatch));
		}
		for (size_t poolIndex = 0; poolIndex < batches.size(); ++poolIndex) {
			IndexTreeMutationPoolBatch &poolBatch = batches[poolIndex];
			const size_t appendedCount = poolIndex < requests.size() ? requests[poolIndex].appended_page_count
																	 : newPoolRequest->appended_page_count;
			poolBatch.appended_page_ids.reserve(appendedCount);
			poolBatch.appended_locations.reserve(appendedCount);
			for (size_t index = 0; index < appendedCount; ++index) {
				const PageId pageId = batch.allocateLogicalPageId();
				// ZYX_COV_EXCL_START: the metadata writer allocates fresh Page IDs for this private B+Tree plan.
				if (replacementIds.contains(pageId) || retirementIds.contains(pageId) ||
					locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation allocated an already reachable Page ID");
				}
				// ZYX_COV_EXCL_STOP
				poolBatch.appended_page_ids.push_back(pageId);
				poolBatch.appended_locations.push_back(
						{pageId, poolBatch.pool.segment_id, 0, poolBatch.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> publishedIds;
		std::set<std::pair<uint64_t, uint16_t>> publishedFrames;
		std::set<std::pair<uint64_t, uint16_t>> retiredFrames;
		std::vector<PageLocation> publishedLocations;
		publishedLocations.reserve(replacementIds.size() + totalAppended);
		std::vector<uint64_t> retiredPageIds;
		retiredPageIds.reserve(retirementIds.size());
		for (const IndexTreeMutationPoolBatch &poolBatch: batches) {
			const uint8_t invalidBatch =
					static_cast<uint8_t>(poolBatch.replacement_locations.size() !=
										 poolBatch.expected_replacements.size()) |
					static_cast<uint8_t>(poolBatch.appended_locations.size() != poolBatch.appended_page_ids.size());
			// ZYX_COV_EXCL_START: the private BTreePageStore preparer preserves batch cardinality.
			if (invalidBatch != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable multi-pool B+Tree preparer changed its batch cardinality");
			}
			// ZYX_COV_EXCL_STOP
			auto validatePublishedLocation = [&](const PageLocation &location, PageId expectedPageId,
																	const CatalogEntry &expectedPool) {
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expectedPageId) |
						static_cast<uint8_t>(location.segment_id != expectedPool.segment_id) |
						static_cast<uint8_t>(location.generation != expectedPool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				// ZYX_COV_EXCL_START: BTreePageStore derives locations from Metadata-assigned unique Page IDs and
				// frames.
				if (invalidLocation != 0U || !publishedIds.insert(location.page_id).second ||
					!publishedFrames.insert(frame).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable multi-pool B+Tree mutation returned invalid or duplicate Page "
										  "Directory locations");
				}
				// ZYX_COV_EXCL_STOP
			};
			for (size_t index = 0; index < poolBatch.replacement_locations.size(); ++index) {
				validatePublishedLocation(poolBatch.replacement_locations[index],
										  poolBatch.expected_replacements[index].page_id,
										  poolBatch.replacement_pool.segment_id.isValid() ? poolBatch.replacement_pool : poolBatch.pool);
				publishedLocations.push_back(poolBatch.replacement_locations[index]);
			}
			for (size_t index = 0; index < poolBatch.appended_locations.size(); ++index) {
				validatePublishedLocation(poolBatch.appended_locations[index], poolBatch.appended_page_ids[index],
										  poolBatch.pool);
				publishedLocations.push_back(poolBatch.appended_locations[index]);
			}
			for (const IndexTreePageRetirement &retirement: poolBatch.expected_retirements) {
				const PageLocation &location = retirement.location;
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				// ZYX_COV_EXCL_START: the private plan cannot both retire and publish one logical page or frame.
				if (publishedIds.contains(location.page_id) || !retiredFrames.insert(frame).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation retirement overlaps a published page or frame");
				}
				// ZYX_COV_EXCL_STOP
				retiredPageIds.push_back(location.page_id.value());
			}
		}

		IndexRootLocation publishedRoot = expectedRoot;
		if (replacementRoot.has_value()) {
			const IndexTreeMutationPoolBatch &rootBatch = batches[replacementRoot->pool_ordinal];
			publishedRoot.root_page_id = rootBatch.appended_locations[replacementRoot->appended_page_ordinal].page_id;
			publishedRoot.root_generation = rootBatch.pool.generation;
		} else if (stableReplacementRoot.has_value()) {
			// ZYX_COV_EXCL_START: erase plans never retain a root that they placed in their retirement set.
			if (retirementIds.contains(*stableReplacementRoot)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable multi-pool B+Tree mutation cannot publish a retired stable root Page ID");
			}
			// ZYX_COV_EXCL_STOP
			const auto location = std::ranges::find(publishedLocations, *stableReplacementRoot, &PageLocation::page_id);
			const PageLocation rootLocation = location == publishedLocations.end()
													  ? locations_.findPage(current, *stableReplacementRoot)
													  : *location;
			const CatalogEntry rootPool = catalog_.find(current, rootLocation.segment_id);
			const uint8_t invalidRoot = static_cast<uint8_t>(rootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										static_cast<uint8_t>(rootPool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(rootPool.generation != rootLocation.generation) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, rootPool.physical_slot));
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable multi-pool B+Tree mutation stable root does not resolve to one "
									  "current index pool");
			}
			publishedRoot.root_page_id = *stableReplacementRoot;
			publishedRoot.root_generation = rootLocation.generation;
		}
		std::ranges::sort(publishedLocations, {}, &PageLocation::page_id);
		std::ranges::sort(retiredPageIds);
		if (retiredPageIds.empty()) {
			locations_.prepareUpsertPages(batch, publishedLocations);
		} else {
			locations_.prepareReplacePages(batch, publishedLocations, retiredPageIds);
		}
		locations_.prepareReplaceIndexRoots(batch, std::span(&publishedRoot, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::appendPageToExistingPool(const CatalogEntry &expectedPool, PageLocation page,
																  LogSequenceNumber checkpointLsn,
																  const ExistingPoolPagePreparer &preparer,
																  const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page append requires a data preparation callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(page.segment_id != visible.segment_id) |
									static_cast<uint8_t>(page.generation != visible.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page append has a stale or unavailable Catalog generation");
		}
		if (!page.page_id.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page append requires a valid Page ID");
		}
		if (locations_.findPageOptional(current, page.page_id).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable existing-pool page append cannot replace an existing Page ID");
		}
		preparer(visible, page);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::appendEntityPageToExistingPool(
			const CatalogEntry &expectedPool, PageLocation page, std::span<const EntityPageLocation> nodes,
			std::span<const EntityPageLocation> edges, LogSequenceNumber checkpointLsn,
			const ExistingPoolPagePreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() || (nodes.empty() == edges.empty())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page append requires one record kind, a checkpoint LSN, and a preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(page.segment_id != visible.segment_id) |
									static_cast<uint8_t>(page.generation != visible.generation) |
									static_cast<uint8_t>(!page.page_id.isValid()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U || locations_.findPageOptional(current, page.page_id).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page append has a stale pool or an already mapped Page ID");
		}
		const auto validateNewLocations = [this, &current, pageId = page.page_id](
												  std::span<const EntityPageLocation> locations, bool nodesMap) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, location.entity_id)
								 : locations_.findEdgePageLocationOptional(current, location.entity_id);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value());
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page append has invalid, duplicate, or published Entity Map entries");
				}
				previous = location.entity_id;
			}
		};
		validateNewLocations(nodes, true);
		validateNewLocations(edges, false);
		preparer(visible, page);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id == PageId{}) |
										static_cast<uint8_t>(page.segment_id != visible.segment_id) |
										static_cast<uint8_t>(page.generation != visible.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page append callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		if (!nodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, {});
		} else {
			locations_.prepareReplaceEdgePageLocations(batch, edges, {});
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::replacePageInExistingPool(const CatalogEntry &expectedPool,
																   const PageLocation &expectedLocation,
																   PageLocation replacement,
																   LogSequenceNumber checkpointLsn,
																   const ExistingPoolPageReplacementPreparer &preparer,
																   const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page replacement requires a data preparation callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page replacement has a stale or unavailable Catalog generation");
		}
		const PageLocation currentLocation = locations_.findPage(current, expectedLocation.page_id);
		const uint8_t invalidLocation =
				static_cast<uint8_t>(currentLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visible.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visible.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index != 0U);
		if (invalidLocation != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page replacement has an invalid current or candidate location");
		}
		preparer(visible, expectedLocation, replacement);
		const uint8_t invalidReplacement =
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_index == expectedLocation.page_index);
		if (invalidReplacement != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable existing-pool page replacement callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&replacement, 1));
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::replaceEntityPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation, PageLocation replacement,
			std::span<const EntityPageLocation> nodes, std::span<const uint64_t> erasedNodeIds,
			std::span<const EntityPageLocation> edges, std::span<const uint64_t> erasedEdgeIds,
			LogSequenceNumber checkpointLsn, const ExistingPoolPageReplacementPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() ||
			((!nodes.empty() || !erasedNodeIds.empty()) && (!edges.empty() || !erasedEdgeIds.empty()))) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page replacement requires one record kind, a checkpoint LSN, and a preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const PageLocation currentLocation = locations_.findPage(current, expectedLocation.page_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
				static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
				static_cast<uint8_t>(visible.kind != expectedPool.kind) |
				static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
				static_cast<uint8_t>(visible.generation != expectedPool.generation) |
				static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
				static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
				static_cast<uint8_t>(currentLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visible.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visible.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index != 0U);
		if (invalidSource != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page replacement has a stale pool, page, or candidate frame");
		}
		const auto validateNewLocations = [this, &current, pageId = expectedLocation.page_id](
												  std::span<const EntityPageLocation> locations, bool nodesMap) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, location.entity_id)
								 : locations_.findEdgePageLocationOptional(current, location.entity_id);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value());
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page replacement has invalid or published Entity Map entries");
				}
				previous = location.entity_id;
			}
		};
		const auto validateErasures = [this, &current, pageId = expectedLocation.page_id](
											  std::span<const uint64_t> entityIds, bool nodesMap) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, entityId)
								 : locations_.findEdgePageLocationOptional(current, entityId);
				const uint8_t invalid = static_cast<uint8_t>(entityId == 0U) |
										static_cast<uint8_t>(entityId <= previous) |
										static_cast<uint8_t>(!existing.has_value()) |
										static_cast<uint8_t>(existing.has_value() && existing->page_id != pageId);
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page replacement erases an absent or foreign Entity Map entry");
				}
				previous = entityId;
			}
		};
		validateNewLocations(nodes, true);
		validateNewLocations(edges, false);
		validateErasures(erasedNodeIds, true);
		validateErasures(erasedEdgeIds, false);
		preparer(visible, expectedLocation, replacement);
		const uint8_t invalidPrepared = static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
										static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
										static_cast<uint8_t>(replacement.generation != visible.generation) |
										static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(replacement.page_index == expectedLocation.page_index);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page replacement callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		locations_.prepareUpsertPages(batch, std::span(&replacement, 1));
		if (!nodes.empty() || !erasedNodeIds.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, erasedNodeIds);
		}
		if (!edges.empty() || !erasedEdgeIds.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, edges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::mutateEntityPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests, LogSequenceNumber checkpointLsn,
			const EntityPageTransactionBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity page transaction requires a preparation callback");
		}
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noNewPropertyPools{};
		return mutateNativeGraphPages(
				requests, appendRequests, newPoolRequests, noPropertyReplacements, noNewPropertyPools, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools,
							std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
							std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools) {
					if (!properties.empty() || !propertyPools.empty()) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable Entity page transaction unexpectedly materialized Property pages");
					}
					preparer(replacements, appends, newPools);
				},
				options);
	}

	Superblock RelocatableMetadataStore::mutateNativeGraphPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			LogSequenceNumber checkpointLsn, const NativeGraphPageTransactionPreparer &preparer,
			const MetadataPublishOptions &options) {
		const NativeGraphDerivedStateMutation noDerivedState{};
		const NativeGraphDerivedStatePreparer noDerivedStatePreparer{};
		return mutateNativeGraphPages(requests, appendRequests, newPoolRequests, propertyReplacements,
									  propertyPageBatchRequests, noDerivedState, checkpointLsn, preparer,
									  noDerivedStatePreparer, options);
	}

	Superblock RelocatableMetadataStore::mutateNativeGraphPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
			const NativeGraphPageTransactionPreparer &preparer,
			const NativeGraphDerivedStatePreparer &derivedStatePreparer, const MetadataPublishOptions &options,
			NativeGraphLogicalIdReservation logicalIdReservation) {
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
		const NativeGraphVectorBootstrapPreparer noVectorBootstrapPreparer{};
		const NativeGraphVectorDeltaPreparer noVectorDeltaPreparer{};
		return mutateNativeGraphPagesWithLargeValues(
				requests, appendRequests, newPoolRequests, propertyReplacements, propertyPageBatchRequests,
				noLargeValues, derivedState, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools,
							std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
							std::span<NativeGraphPropertyPageBatchMaterialization> newProperties,
							std::span<LargeValueMaterializationResult> largeValues) {
					if (!largeValues.empty()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph page transaction unexpectedly materialized LargeValues");
					}
					preparer(replacements, appends, newPools, properties, newProperties);
				},
				derivedStatePreparer, noVectorBootstrapPreparer, noVectorDeltaPreparer, {}, {}, {}, options,
				logicalIdReservation);
	}


	Superblock RelocatableMetadataStore::mutateEntityPagesInExistingPools(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests, LogSequenceNumber checkpointLsn,
			const EntityPageMutationBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool Entity page mutation requires a preparation callback");
		}
		const std::array<EntityPageNewPoolBatchRequest, 0U> noNewPools{};
		return mutateEntityPages(
				requests, appendRequests, noNewPools, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools) {
					if (!newPools.empty()) { // this wrapper always passes a zero-length new-pool request array.
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable existing-pool Entity page mutation unexpectedly allocated a new pool");
					}
					preparer(replacements, appends);
				},
				options);
	}

	Superblock RelocatableMetadataStore::replaceEntityPagesInExistingPools(
			std::span<const EntityPageReplacementRequest> requests, LogSequenceNumber checkpointLsn,
			const EntityPageReplacementBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity page batch replacement requires a preparation callback");
		}
		const std::array<EntityPageAppendBatchRequest, 0> noAppends{};
		return mutateEntityPagesInExistingPools(
				requests, noAppends, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends) {
					if (!appends.empty()) { // this replacement-only wrapper always passes a zero-length append request array.
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable Entity page replacement unexpectedly received append materializations");
					}
					preparer(replacements);
				},
				options);
	}

	Superblock RelocatableMetadataStore::retireEntityPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation,
			std::span<const uint64_t> erasedNodeIds, std::span<const uint64_t> erasedEdgeIds,
			LogSequenceNumber checkpointLsn, const ExistingPoolPageRetirementPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() || (erasedNodeIds.empty() == erasedEdgeIds.empty())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page retirement requires one record kind, a checkpoint LSN, and a preparer");
		}
		const auto validateErasures = [](std::span<const uint64_t> entityIds) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				if (entityId == 0U || entityId <= previous) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "relocatable Entity page retirement requires canonical Entity IDs");
				}
				previous = entityId;
			}
		};
		validateErasures(erasedNodeIds);
		validateErasures(erasedEdgeIds);

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const PageLocation currentLocation = locations_.findPage(current, expectedLocation.page_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
				static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
				static_cast<uint8_t>(visible.kind != expectedPool.kind) |
				static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
				static_cast<uint8_t>(visible.generation != expectedPool.generation) |
				static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
				static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
				static_cast<uint8_t>(currentLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visible.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visible.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidSource != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page retirement has a stale pool, page, or physical frame");
		}
		const auto validateCurrentMappings = [this, &current, pageId = expectedLocation.page_id](
													 std::span<const uint64_t> entityIds, bool nodesMap) {
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> mapping =
						nodesMap ? locations_.findNodePageLocationOptional(current, entityId)
								 : locations_.findEdgePageLocationOptional(current, entityId);
				if (!mapping.has_value() || mapping->page_id != pageId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page retirement would erase an absent or foreign Entity Map entry");
				}
			}
		};
		validateCurrentMappings(erasedNodeIds, true);
		validateCurrentMappings(erasedEdgeIds, false);
		preparer(visible, expectedLocation);

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::array<PageLocation, 0> noPages{};
		const std::array<uint64_t, 1> erasedPageIds{{expectedLocation.page_id.value()}};
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		if (!erasedNodeIds.empty()) {
			const std::array<EntityPageLocation, 0> noNodes{};
			locations_.prepareReplaceNodePageLocations(batch, noNodes, erasedNodeIds);
		} else {
			const std::array<EntityPageLocation, 0> noEdges{};
			locations_.prepareReplaceEdgePageLocations(batch, noEdges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retireVectorManifestPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation,
			const VectorManifestLocation &expectedManifest, LogSequenceNumber checkpointLsn,
			const ExistingPoolPageRetirementPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput =
				static_cast<uint8_t>(!expectedManifest.index_id) |
				static_cast<uint8_t>(!expectedManifest.manifest_page_id.isValid()) |
				static_cast<uint8_t>(!expectedManifest.generation.isValid()) |
				static_cast<uint8_t>(expectedManifest.manifest_page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(expectedManifest.generation != expectedLocation.generation) |
				static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest retirement requires one valid current mapping and preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visiblePool = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visiblePool.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visiblePool.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visiblePool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
									static_cast<uint8_t>(visiblePool.kind != expectedPool.kind) |
									static_cast<uint8_t>(visiblePool.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visiblePool.generation != expectedPool.generation) |
									static_cast<uint8_t>(visiblePool.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visiblePool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visiblePool.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest retirement has a stale or unavailable dedicated pool");
		}
		const PageLocation visibleLocation = locations_.findPage(current, expectedLocation.page_id);
		const std::optional<VectorManifestLocation> visibleManifest =
				locations_.findVectorManifestOptional(current, expectedManifest.index_id);
		const uint8_t invalidMapping =
				static_cast<uint8_t>(visibleLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visiblePool.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visiblePool.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(!visibleManifest.has_value()) |
				static_cast<uint8_t>(visibleManifest.has_value() && *visibleManifest != expectedManifest);
		if (invalidMapping != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest retirement has a stale Page or Manifest Directory mapping");
		}
		preparer(visiblePool, visibleLocation);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::array<PageLocation, 0> noPages{};
		const std::array<uint64_t, 1> erasedPageIds{expectedLocation.page_id.value()};
		const std::array<VectorManifestLocation, 0> noManifests{};
		const std::array<uint64_t, 1> erasedIndexIds{expectedManifest.index_id};
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		locations_.prepareReplaceVectorManifests(batch, noManifests, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retirePagesInExistingPools(Generation expectedMetadataGeneration,
																						std::span<const IndexRootLocation> erasedRoots,
																						std::span<const PageRetirement> retirements,
																						LogSequenceNumber checkpointLsn,
																						const ExistingPoolPagesRetirementPreparer &preparer,
																						const MetadataPublishOptions &options,
																						std::span<const uint64_t> erasedVectorIds) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedMetadataGeneration.isValid()) |
									 static_cast<uint8_t>(retirements.empty()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable page retirement requires a scan generation, nonempty plan, checkpoint "
								  "LSN, and preparer");
		}

		uint64_t previousPageId = 0U;
		for (const PageRetirement &retirement: retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) |
					static_cast<uint8_t>(retirement.expected_owner_kind == PageOwnerKind::POK_NONE) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid());
			if (invalidRetirement != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has an invalid or non-canonical source frame");
			}
			previousPageId = location.page_id.value();
		}
		uint64_t previousRootIndexId = 0U;
		for (const IndexRootLocation &root: erasedRoots) {
			const uint8_t invalidRoot = static_cast<uint8_t>(root.index_id == 0U) |
												static_cast<uint8_t>(root.index_id <= previousRootIndexId) |
												static_cast<uint8_t>(!root.root_page_id.isValid()) |
												static_cast<uint8_t>(!root.root_generation.isValid());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has an invalid or non-canonical erased Index Root");
			}
			previousRootIndexId = root.index_id;
		}
		uint64_t previousVectorId = 0U;
		for (const uint64_t vectorId: erasedVectorIds) {
			if (vectorId == 0U || vectorId <= previousVectorId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has non-canonical erased Vector identities");
			}
			previousVectorId = vectorId;
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.generation != expectedMetadataGeneration) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable page retirement scan generation is stale and must be repeated");
		}
		for (const IndexRootLocation &root: erasedRoots) {
			if (locations_.findIndexRoot(current, root.index_id) != root) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable page retirement has a stale or replaced Index Root");
			}
		}
		std::map<uint64_t, ExistingPoolPageRetirementBatch> batchesBySegment;
		for (const PageRetirement &retirement: retirements) {
			const PageLocation visibleLocation = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const bool largeValueData = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA;
			const bool largeValueManifest = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(visibleLocation != retirement.location) |
					static_cast<uint8_t>(pool.segment_id != retirement.location.segment_id) |
					static_cast<uint8_t>(pool.generation != retirement.location.generation) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isSharedPagePoolSegmentKind(pool.kind)) |
					static_cast<uint8_t>(!matchesPageDirectoryOnlyPoolOwnerKind(pool.kind, retirement.expected_owner_kind)) |
					static_cast<uint8_t>(largeValueData || largeValueManifest) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable page retirement has a stale, unavailable, or incompatible source frame");
			}
			auto [iterator, inserted] = batchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}

		std::vector<ExistingPoolPageRetirementBatch> batches;
		batches.reserve(batchesBySegment.size());
		for (auto &[segmentId, batch]: batchesBySegment) {
			static_cast<void>(segmentId);
			batches.push_back(std::move(batch));
		}
		preparer(batches);

		std::vector<uint64_t> erasedPageIds;
		erasedPageIds.reserve(retirements.size());
		for (const PageRetirement &retirement: retirements) {
			erasedPageIds.push_back(retirement.location.page_id.value());
		}
		if (!erasedVectorIds.empty()) {
			const std::set<PageId> retiredPages = [&retirements] {
				std::set<PageId> result;
				for (const PageRetirement &retirement: retirements) {
					result.insert(retirement.location.page_id);
				}
				return result;
			}();
			for (const uint64_t vectorId: erasedVectorIds) {
				const VectorLocation location = locations_.findVectorLocation(current, vectorId);
				if (!retiredPages.contains(location.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable page retirement would erase a Vector Location outside its retired pages");
				}
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::vector<PageLocation> noPages;
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		std::vector<uint64_t> erasedRootIndexIds;
		erasedRootIndexIds.reserve(erasedRoots.size());
		for (const IndexRootLocation &root: erasedRoots) {
			erasedRootIndexIds.push_back(root.index_id);
		}
		if (!erasedRootIndexIds.empty()) {
			const std::array<IndexRootLocation, 0> noRoots{};
			locations_.prepareReplaceIndexRoots(batch, noRoots, erasedRootIndexIds);
		}
		if (!erasedVectorIds.empty()) {
			const std::array<VectorLocation, 0> noVectorLocations{};
			locations_.prepareReplaceVectorLocations(batch, noVectorLocations, erasedVectorIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retireVectorCodebookExtents(Generation expectedMetadataGeneration,
																	 std::span<const ExtentRetirement> retirements,
																	 LogSequenceNumber checkpointLsn,
																	 const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedMetadataGeneration.isValid()) |
									 static_cast<uint8_t>(retirements.empty()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Codebook retirement requires a scan generation, nonempty plan, "
								  "and checkpoint LSN");
		}
		uint64_t previousExtentId = 0U;
		std::set<uint64_t> segmentIds;
		for (const ExtentRetirement &retirement: retirements) {
			const ExtentLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.extent_id.isValid()) |
					static_cast<uint8_t>(location.extent_id.value() <= previousExtentId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) |
					static_cast<uint8_t>(!segmentIds.insert(location.segment_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable Vector Codebook retirement has an invalid or non-canonical extent plan");
			}
			previousExtentId = location.extent_id.value();
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.generation != expectedMetadataGeneration) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Codebook retirement scan generation is stale and must be repeated");
		}
		std::vector<CatalogEntry> retiredEntries;
		std::vector<uint64_t> erasedExtentIds;
		retiredEntries.reserve(retirements.size());
		erasedExtentIds.reserve(retirements.size());
		for (const ExtentRetirement &retirement: retirements) {
			const ExtentLocation visibleLocation = locations_.findExtent(current, retirement.location.extent_id);
			const CatalogEntry visibleSegment = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(visibleLocation != retirement.location) |
					static_cast<uint8_t>(visibleSegment.segment_id != retirement.location.segment_id) |
					static_cast<uint8_t>(visibleSegment.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(visibleSegment.owner_id != retirement.expected_owner_id) |
					static_cast<uint8_t>(visibleSegment.generation != retirement.location.generation) |
					static_cast<uint8_t>(visibleSegment.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visibleSegment.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Vector Codebook retirement has a stale or unavailable extent mapping");
			}
			CatalogEntry retired = visibleSegment;
			retired.state = SegmentState::SGS_RETIRED;
			retiredEntries.push_back(retired);
			erasedExtentIds.push_back(retirement.location.extent_id.value());
		}
		std::ranges::sort(retiredEntries, {}, &CatalogEntry::segment_id);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const CatalogEntry &retired: retiredEntries) {
			batch.reserveDataSlot(retired.physical_slot);
		}
		catalog_.prepareUpsert(batch, retiredEntries);
		std::vector<RetireQueueEntry> queued;
		queued.reserve(retiredEntries.size());
		for (const CatalogEntry &retired: retiredEntries) {
			queued.push_back({retired.physical_slot, retired.segment_id, batch.generation(),
							  RetireQueueDisposition::RQD_RETIRED_SEGMENT});
		}
		std::ranges::sort(queued, {}, &RetireQueueEntry::physical_slot);
		retirement_.prepareQueueInsertions(batch, queued);
		const std::array<ExtentLocation, 0> noExtents{};
		locations_.prepareReplaceExtents(batch, noExtents, erasedExtentIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::evacuatePageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination, LogSequenceNumber checkpointLsn,
			const CrossPoolPageEvacuationPreparer &preparer, const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, {}, {}, {}, {}, checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateIndexRootPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const IndexRootLocation &expectedRoot, const IndexRootLocation &replacementRoot,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, expectedRoot, replacementRoot, {}, {}, {}, {}, checkpointLsn,
										   preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateVectorManifestPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const VectorManifestLocation &expectedManifest, const VectorManifestLocation &replacementManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, expectedManifest, replacementManifest, {}, {},
										   checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateLargeValueManifestPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const LargeValueManifestLocation &expectedManifest, const LargeValueManifestLocation &replacementManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, {}, {}, expectedManifest, replacementManifest,
										   checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuatePageAcrossPoolsImpl(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const std::optional<IndexRootLocation> &expectedRoot,
			const std::optional<IndexRootLocation> &replacementRoot,
			const std::optional<VectorManifestLocation> &expectedManifest,
			const std::optional<VectorManifestLocation> &replacementManifest,
			const std::optional<LargeValueManifestLocation> &expectedLargeValueManifest,
			const std::optional<LargeValueManifestLocation> &replacementLargeValueManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		// ZYX_COV_EXCL_START: the typed wrappers pass either no rebind or one complete directory pair,
		// or one complete Manifest pair; this private implementation is never exposed with half-pairs.
		const bool rootRebind = expectedRoot.has_value() || replacementRoot.has_value();
		const bool manifestRebind = expectedManifest.has_value() || replacementManifest.has_value();
		const bool largeValueManifestRebind =
				expectedLargeValueManifest.has_value() || replacementLargeValueManifest.has_value();
		if (rootRebind && (!expectedRoot.has_value() || !replacementRoot.has_value())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree root evacuation requires both current and replacement root mappings");
		}
		if (manifestRebind && (!expectedManifest.has_value() || !replacementManifest.has_value())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest evacuation requires both current and replacement directory mappings");
		}
		if (largeValueManifestRebind &&
			(!expectedLargeValueManifest.has_value() || !replacementLargeValueManifest.has_value())) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable LargeValue Manifest evacuation requires both current and replacement "
								  "directory mappings");
		}
		const uint8_t rebindCount = static_cast<uint8_t>(rootRebind) + static_cast<uint8_t>(manifestRebind) +
									static_cast<uint8_t>(largeValueManifestRebind);
		if (rebindCount > 1U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool evacuation cannot rebind multiple typed directories together");
		}
		// ZYX_COV_EXCL_STOP
		if (!preparer || !expectedSourcePool.segment_id.isValid() || !expectedDestinationPool.segment_id.isValid() ||
			expectedSourcePool.segment_id == expectedDestinationPool.segment_id) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool page evacuation requires distinct valid pools and a callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry source = catalog_.find(current, expectedSourcePool.segment_id);
		const CatalogEntry target = catalog_.find(current, expectedDestinationPool.segment_id);
		const auto isExpectedSealedPool = [this, &current](const CatalogEntry &visible, const CatalogEntry &expected) {
			const uint8_t invalid = static_cast<uint8_t>(visible.segment_id != expected.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expected.physical_slot) |
									static_cast<uint8_t>(visible.kind != expected.kind) |
									static_cast<uint8_t>(visible.owner_id != expected.owner_id) |
									static_cast<uint8_t>(visible.generation != expected.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expected.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
			return invalid == 0U;
		};
		if (!isExpectedSealedPool(source, expectedSourcePool) ||
			!isExpectedSealedPool(target, expectedDestinationPool) || source.physical_slot == target.physical_slot ||
			source.kind != target.kind) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation has a stale, unavailable, or incompatible pool");
		}
		const PageLocation currentLocation = locations_.findPage(current, expectedSourceLocation.page_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(currentLocation != expectedSourceLocation) |
				static_cast<uint8_t>(expectedSourceLocation.segment_id != source.segment_id) |
				static_cast<uint8_t>(expectedSourceLocation.generation != source.generation) |
				static_cast<uint8_t>(expectedSourceLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedSourceLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(destination.page_id != expectedSourceLocation.page_id) |
				static_cast<uint8_t>(destination.segment_id != target.segment_id) |
				static_cast<uint8_t>(destination.generation != target.generation) |
				static_cast<uint8_t>(destination.page_index != 0U);
		if (invalidSource != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation has an invalid source or destination location");
		}
		if (rootRebind) {
			const std::optional<IndexRootLocation> currentRoot =
					locations_.findIndexRootOptional(current, expectedRoot->index_id);
			const uint8_t invalidRoot =
					static_cast<uint8_t>(expectedRoot->index_id == 0U) |
					static_cast<uint8_t>(expectedRoot->root_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedRoot->root_generation != source.generation) |
					static_cast<uint8_t>(replacementRoot->index_id != expectedRoot->index_id) |
					static_cast<uint8_t>(replacementRoot->root_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementRoot->root_generation != target.generation) |
					static_cast<uint8_t>(!currentRoot.has_value()) |
					static_cast<uint8_t>(currentRoot.has_value() && *currentRoot != *expectedRoot);
			if (invalidRoot != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable B+Tree root evacuation has a stale or inconsistent Index Root mapping");
			}
		}
		if (manifestRebind) {
			const std::optional<VectorManifestLocation> currentManifest =
					locations_.findVectorManifestOptional(current, expectedManifest->index_id);
			const uint8_t invalidManifest =
					static_cast<uint8_t>(expectedManifest->index_id == 0U) |
					static_cast<uint8_t>(expectedManifest->manifest_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedManifest->generation != source.generation) |
					static_cast<uint8_t>(replacementManifest->index_id != expectedManifest->index_id) |
					static_cast<uint8_t>(replacementManifest->manifest_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementManifest->generation != target.generation) |
					static_cast<uint8_t>(!currentManifest.has_value()) |
					static_cast<uint8_t>(currentManifest.has_value() && *currentManifest != *expectedManifest);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector Manifest evacuation has a stale or inconsistent Manifest "
									  "Directory mapping");
			}
		}
		if (largeValueManifestRebind) {
			const std::optional<LargeValueManifestLocation> currentManifest =
					locations_.findLargeValueManifestOptional(current, expectedLargeValueManifest->value_id);
			const uint8_t invalidManifest =
					static_cast<uint8_t>(!expectedLargeValueManifest->value_id.isValid()) |
					static_cast<uint8_t>(expectedLargeValueManifest->manifest_page_id !=
										 expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedLargeValueManifest->generation != source.generation) |
					static_cast<uint8_t>(replacementLargeValueManifest->value_id !=
										 expectedLargeValueManifest->value_id) |
					static_cast<uint8_t>(replacementLargeValueManifest->manifest_page_id !=
										 expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementLargeValueManifest->generation != target.generation) |
					static_cast<uint8_t>(!currentManifest.has_value()) |
					static_cast<uint8_t>(currentManifest.has_value() &&
										 *currentManifest != *expectedLargeValueManifest);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue Manifest evacuation has a stale or inconsistent Manifest "
									  "Directory mapping");
			}
		}
		preparer(source, expectedSourceLocation, target, destination);
		const uint8_t invalidDestination =
				static_cast<uint8_t>(destination.page_id != expectedSourceLocation.page_id) |
				static_cast<uint8_t>(destination.segment_id != target.segment_id) |
				static_cast<uint8_t>(destination.generation != target.generation) |
				static_cast<uint8_t>(destination.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(destination.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidDestination != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation callback returned an invalid destination frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&destination, 1));
		if (rootRebind) {
			locations_.prepareReplaceIndexRoots(batch, std::span(&*replacementRoot, 1), {});
		}
		if (manifestRebind) {
			locations_.prepareReplaceVectorManifests(batch, std::span(&*replacementManifest, 1), {});
		}
		if (largeValueManifestRebind) {
			locations_.prepareReplaceLargeValueManifests(batch, std::span(&*replacementLargeValueManifest, 1), {});
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	void RelocatableMetadataStore::reconcileExistingPool(SegmentId segmentId,
														 const ExistingPoolReconciler &reconciler) {
		if (!segmentId.isValid() || !reconciler) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable pool reconciliation requires a valid Segment ID and callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, segmentId);
		const uint8_t unavailable = static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (unavailable != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable pool reconciliation requires one allocated sealed Catalog generation");
		}
		const auto findPage = [this, &current](PageId pageId) -> std::optional<PageLocation> {
			return locations_.findPageOptional(current, pageId);
		};
		const auto findCatalog = [this, &current](SegmentId targetSegmentId) {
			const CatalogEntry target = catalog_.find(current, targetSegmentId);
			const uint8_t unavailable = static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, target.physical_slot));
			if (unavailable != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable remote pool reconciliation target is not current and allocated");
			}
			return target;
		};
		const ExistingPoolFrameVisibility isFrameVisibleToActiveSnapshot =
				[this, &writer](PageId pageId, SegmentId segmentId, uint16_t pageIndex, Generation generation) {
					return snapshots_.anyActiveSnapshot(writer, [this, pageId, segmentId, pageIndex,
																 generation](const Superblock &snapshot) {
						const std::optional<PageLocation> location = locations_.findPageOptional(snapshot, pageId);
						return location.has_value() && location->segment_id == segmentId &&
							   location->page_index == pageIndex && location->generation == generation;
					});
				};
		reconciler(visible, findPage, findCatalog, isFrameVisibleToActiveSnapshot);
	}

	void RelocatableMetadataStore::reconcileCurrentPools(const CurrentPoolReconciler &reconciler) {
		if (!reconciler) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable current-pool reconciliation requires a callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		std::map<uint64_t, std::vector<PageLocation>> pagesBySegment;
		locations_.visitPages(current, [this, &current, &pagesBySegment](const PageLocation &location) {
			const CatalogEntry target = catalog_.find(current, location.segment_id);
			const bool isSharedPool = isSharedPagePoolSegmentKind(target.kind);
			const uint8_t invalidTarget = static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
										  static_cast<uint8_t>(target.generation != location.generation) |
										  static_cast<uint8_t>(!isSharedPool) |
										  static_cast<uint8_t>(!allocator_.isAllocated(current, target.physical_slot));
			if (invalidTarget != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Page Directory entry does not target a current allocated pool");
			}
			pagesBySegment[location.segment_id.value()].push_back(location);
		});
		catalog_.visit(current, [this, &current, &pagesBySegment, &reconciler](const CatalogEntry &visible) {
			if (visible.state != SegmentState::SGS_SEALED) {
				return;
			}
			if (!allocator_.isAllocated(current, visible.physical_slot)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable sealed metadata pool has no free-space ownership");
			}
			const auto pages = pagesBySegment.find(visible.segment_id.value());
			if (pages == pagesBySegment.end()) {
				reconciler(visible, {});
				return;
			}
			reconciler(visible, pages->second);
		});
	}

	Superblock RelocatableMetadataStore::upsertLocationsImpl(std::span<const PageLocation> pages,
															 std::span<const ExtentLocation> extents,
															 LogSequenceNumber checkpointLsn,
															 const LocationPublicationPreparer *preparer,
															 const MetadataPublishOptions &options) {
		if (pages.empty() && extents.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable location publication requires a Page or Extent mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (!pages.empty()) {
			locations_.prepareUpsertPages(batch, pages);
		}
		if (!extents.empty()) {
			locations_.prepareUpsertExtents(batch, extents);
		}
		if (preparer != nullptr) {
			(*preparer)();
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::setPhysicalSlotAllocated(PhysicalSlotId slot, bool allocated,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (!allocated && metadata_page_pool_.isMetadataPoolSlot(slot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata page pool ownership cannot be released through free-space mutation");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (allocated) {
			batch.reserveDataSlot(slot);
		}
		allocator_.prepareSetAllocated(batch, slot, allocated);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<AllocatorReservation>
	RelocatableMetadataStore::reserveFirstFreeSlot(PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
												   LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return AllocatorReservation{batch.candidate(), *physicalSlot};
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot, checkpointLsn,
													nullptr, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable prepared Catalog reservation requires a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot, checkpointLsn,
													&preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndLocations(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const ExtentLocation> extents, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (pages.empty() && extents.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable combined Catalog and location reservation requires a location mutation");
		}
		if (!preparer) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable combined Catalog and location reservation requires a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, extents, {}, {}, {}, firstSlot, lastSlot,
													checkpointLsn, &preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndVectorManifests(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const VectorManifestLocation> manifests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (pages.empty() || manifests.empty() || !preparer) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest reservation needs pages, manifests, and a data preparer");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, {}, {}, {}, manifests, firstSlot, lastSlot,
													checkpointLsn, &preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations>
	RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
			std::span<const CatalogSlotReservationRequest> requests, std::span<PageLocation> pages,
			std::span<const EntityPageLocation> nodes, std::span<const EntityPageLocation> edges,
			PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
			const EntityPagePoolReservationPreparer &preparer, const MetadataPublishOptions &options) {
		if (pages.empty() || (nodes.empty() == edges.empty()) || !preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Entity page reservation needs pages, "
																	"one record kind, and a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, {}, nodes, edges, {}, firstSlot, lastSlot,
													checkpointLsn, nullptr, &preparer, pages, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogImpl(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const ExtentLocation> extents, std::span<const EntityPageLocation> entityPageNodes,
			std::span<const EntityPageLocation> entityPageEdges, std::span<const VectorManifestLocation> manifests,
			PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
			const CatalogSlotReservationPreparer *preparer, const EntityPagePoolReservationPreparer *entityPagePreparer,
			std::span<PageLocation> mutableEntityPages, const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const CatalogReservationPublicationRequest request{
				requests,  pages,	 extents,  entityPageNodes,	   entityPageEdges,	  manifests,
				firstSlot, lastSlot, preparer, entityPagePreparer, mutableEntityPages};
		std::optional<std::vector<CatalogEntry>> entries = reservation_coordinator_.prepare(batch, request);
		if (!entries.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotReservations{batch.candidate(), std::move(*entries)};
	}

	std::optional<CatalogSlotReservation> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			CatalogSlotReservationRequest request, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		const std::optional<CatalogSlotReservations> reservations =
				reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), firstSlot, lastSlot, checkpointLsn, options);
		if (!reservations.has_value()) {
			return std::nullopt;
		}
		return CatalogSlotReservation{reservations->superblock, reservations->entries.front()};
	}

	Superblock RelocatableMetadataStore::retireCatalogSegment(SegmentId segmentId, Generation expectedGeneration,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options) {
		const std::array requests{CatalogSegmentRetirementRequest{segmentId, expectedGeneration}};
		return retireCatalogSegments(requests, checkpointLsn, options);
	}

	Superblock
	RelocatableMetadataStore::retireCatalogSegments(std::span<const CatalogSegmentRetirementRequest> requests,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		if (requests.empty() || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog retirement batch requires segments and a checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_retirement_coordinator_.prepareRetirements(batch, requests);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::releaseQueryScratchSegments(
		std::span<const CatalogSegmentRetirementRequest> requests, uint64_t owner,
		LogSequenceNumber checkpoint, const MetadataPublishOptions &options) {
		if(requests.empty() || requests.size()>64U || owner==0U || !checkpoint.isValid())
			throw FormatException(FormatError::FERR_INVALID_HEADER,"query scratch release requires bounded owner identities");
		MetadataWriteGuard writer=beginWrite();const Superblock current=bootstrap_.recover();
		std::vector<CatalogEntry> released;released.reserve(requests.size());std::set<uint64_t> ids;
		for(const auto &request:requests){
			const auto entry=catalog_.find(current,request.segment_id);
			if(!ids.insert(entry.segment_id.value()).second || entry.kind!=SegmentKind::SGK_QUERY_SCRATCH ||
				entry.owner_id!=owner || entry.generation!=request.expected_generation || entry.state!=SegmentState::SGS_SEALED ||
				!allocator_.isAllocated(current,entry.physical_slot))
				throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch release rejects stale, foreign or user segments");
			auto obsolete=entry;obsolete.state=SegmentState::SGS_OBSOLETE;released.push_back(obsolete);
		}
		// A corrupted registration must not turn public data into disposable scratch.
		locations_.visitPages(current,[&](const PageLocation &page){if(ids.contains(page.segment_id.value()))
			throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch is reachable from a public Page Directory");});
		locations_.visitExtents(current,[&](const ExtentLocation &extent){if(ids.contains(extent.segment_id.value()))
			throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch is reachable from a public Extent Directory");});
		MetadataWriteBatch batch(metadata_page_pool_,current,checkpoint);
		std::ranges::sort(released,{},&CatalogEntry::segment_id);catalog_.prepareUpsert(batch,released);
		for(const auto &entry:released){batch.reserveDataSlot(entry.physical_slot);allocator_.prepareSetAllocated(batch,entry.physical_slot,false);}
		publishBatch(batch,writer,options);return batch.candidate();
	}

	Superblock RelocatableMetadataStore::releaseRetiredCatalogSegment(SegmentId segmentId,
																	  Generation expectedGeneration,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
        auto result = tryReleaseRetiredCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
        if (!result) throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable retired slot release is blocked by a snapshot that can resolve the retired slot");
        return *result;
    }

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRetiredCatalogSegment(SegmentId segmentId,
																	  Generation expectedGeneration,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const RetireQueueEntry retirement =
				segment_retirement_coordinator_.validateRetiredRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
            return std::nullopt;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_retirement_coordinator_.prepareRetiredRelease(batch, retirement);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRetiredCatalogSegments(
			std::span<const CatalogSegmentRetirementRequest> requests, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		if (requests.empty() || !checkpointLsn.isValid())
			throw FormatException(FormatError::FERR_INVALID_HEADER,
				"relocatable retired slot release requires a nonempty batch and checkpoint LSN");
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		utils::OwnedVector<uint64_t> identities;
		identities.reserve(requests.size());
		for (const auto &request : requests) {
			if (!request.segment_id.isValid() || !request.expected_generation.isValid())
				throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable retired slot release has an invalid segment identity");
			identities.push_back(request.segment_id.value());
		}
		std::ranges::sort(identities);
		if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
			throw FormatException(FormatError::FERR_INVALID_HEADER,
				"relocatable retired slot release has a duplicate segment");
		utils::OwnedVector<RetireQueueEntry> retirements;
		retirements.reserve(requests.size());
		bool blocked = false;
		for (const auto &request : requests) {
			const auto retirement = segment_retirement_coordinator_.validateRetiredRelease(
				current, request.segment_id, request.expected_generation);
			blocked = retirement_.isVisibleToActiveSnapshot(writer, retirement) || blocked;
			retirements.push_back(retirement);
		}
		if (blocked) return std::nullopt;
		std::ranges::sort(retirements, {}, &RetireQueueEntry::physical_slot);
        // A reused slot must not retain one permanent Catalog owner per release.
        // Only older, already-obsolete shared-pool owners of these exact slots
        // are eligible. Two old owners per released slot bound this transaction
        // and gradually drain history created by earlier engine versions.
        if (requests.size() > std::numeric_limits<size_t>::max() / 2U)
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
                "relocatable Catalog history cleanup bound overflow");
        const size_t maximumHistory = requests.size() * 2U;
        utils::OwnedVector<SegmentId> obsoleteOwners;
        obsoleteOwners.reserve(maximumHistory);
        catalog_.visit(current, [&](const CatalogEntry &entry) {
            if (obsoleteOwners.size() == maximumHistory || entry.state != SegmentState::SGS_OBSOLETE ||
                !isSharedPagePoolSegmentKind(entry.kind)) return;
            const auto selected = std::lower_bound(retirements.begin(), retirements.end(), entry.physical_slot,
                [](const RetireQueueEntry &retirement, PhysicalSlotId slot) { return retirement.physical_slot < slot; });
            if (selected != retirements.end() && selected->physical_slot == entry.physical_slot)
                obsoleteOwners.push_back(entry.segment_id);
        });
        if (!obsoleteOwners.empty()) {
            std::ranges::sort(obsoleteOwners);
            locations_.visitPages(current, [&](const PageLocation &location) {
                if (std::binary_search(obsoleteOwners.begin(), obsoleteOwners.end(), location.segment_id))
                    throw FormatException(FormatError::FERR_INVALID_STATE,
                        "relocatable obsolete Catalog history still has a current Page Directory reference");
            });
        }
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		retire_queue_reclaimer_.prepareReclamation(batch, retirements);
        if (!obsoleteOwners.empty()) catalog_.prepareErase(batch, obsoleteOwners);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<CatalogSlotRelocation> RelocatableMetadataStore::relocateCatalogSegment(
			SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotRelocationPreparer &preparer,
			const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<CatalogSlotRelocationPlan> plan = segment_relocation_coordinator_.prepareSegmentRelocation(
				batch, segmentId, expectedGeneration, firstSlot, lastSlot, preparer);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotRelocation{batch.candidate(), plan->source, plan->destination};
	}

	std::optional<CatalogSlotRelocation> RelocatableMetadataStore::relocateCatalogExtentSegment(
			const ExtentLocation &expectedExtent, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotRelocationPreparer &preparer,
			const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<CatalogSlotRelocationPlan> plan = segment_relocation_coordinator_.prepareExtentRelocation(
				batch, expectedExtent, firstSlot, lastSlot, preparer);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotRelocation{batch.candidate(), plan->source, plan->destination};
	}

	Superblock RelocatableMetadataStore::releaseRelocatedCatalogSource(SegmentId segmentId,
																	   Generation expectedGeneration,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options) {
        auto result = tryReleaseRelocatedCatalogSource(segmentId, expectedGeneration, checkpointLsn, options);
        if (!result) throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable relocated source release is blocked by a snapshot that can resolve the source");
        return *result;
    }

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRelocatedCatalogSource(SegmentId segmentId,
																	   Generation expectedGeneration,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const RetireQueueEntry retirement =
				segment_relocation_coordinator_.validateRelocatedSourceRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
            return std::nullopt;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_relocation_coordinator_.prepareRelocatedSourceRelease(batch, retirement);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	void RelocatableMetadataStore::collectMetadataGarbage() {
		MetadataWriteGuard writer = beginWrite();
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, bootstrap_.recover());
	}

	MetadataPagePoolReclamation
	RelocatableMetadataStore::reclaimEmptyMetadataPagePools(LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot reclaim metadata page pools");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		// This reconciles page-frame bitmaps against current and pinned roots while
		// the writer barrier prevents a new snapshot from observing an unprotected
		// frame. A selected pool therefore has no metadata frame reachable from any
		// generation that can still read this process.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);

		utils::OwnedVector<CatalogEntry> candidates;
		catalog_.visit(current, [this, &candidates, &current](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL || entry.state == SegmentState::SGS_OBSOLETE) {
				return;
			}
			const uint8_t invalid = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(entry.owner_id != entry.physical_slot.value()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot)) |
									static_cast<uint8_t>(!metadata_page_pool_.isMetadataPoolSlot(entry.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool reclaimer found invalid durable ownership");
			}
			const MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(entry.physical_slot);
			if (header.segment_id != entry.segment_id || header.generation != entry.generation ||
				header.committed_lsn != entry.committed_lsn) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool reclaimer found a Catalog/header identity mismatch");
			}
			if (metadata_page_pool_.isMetadataPoolEmpty(entry.physical_slot)) {
				candidates.push_back(entry);
			}
		});
        bool pruneRoutes = false;
        if (current.usesPageDirectoryBootstrap()) {
            MetadataPageRouteDirectory routes(pages_);
            const auto header = pages_.readPageDirectoryBootstrap(current.page_directory_root);
            // Validate the complete old branch before discarding detached targets.
            const auto registered = routes.collectRoutesFromRouteBranch(current, header.metadata_page_directory_root);
            auto physical = current; physical.page_directory_root = header.data_page_directory_root;
            const auto live = routes.collectRoutesFromPhysicalDirectoryRoots(physical);
            pruneRoutes = registered.size() > live.size();
        }
		if (candidates.empty() && !pruneRoutes) {
			return MetadataPagePoolReclamation{current, 0U, {}};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        if (pruneRoutes) batch.requestMetadataRoutePruning();
		utils::OwnedStdVector<PhysicalSlotId> candidateSlots;
		utils::OwnedVector<SegmentId> candidateSegmentIds;
		candidateSlots.reserve(candidates.size());
		candidateSegmentIds.reserve(candidates.size());
		for (const CatalogEntry &entry: candidates) {
			candidateSlots.push_back(entry.physical_slot);
			candidateSegmentIds.push_back(entry.segment_id);
		}
		std::ranges::sort(candidateSlots);
		if (std::ranges::adjacent_find(candidateSlots) != candidateSlots.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool reclaimer found duplicate physical-slot ownership");
		}
		// Reserve every selected pool before allocator/Catalog COW can allocate a
		// metadata frame. Reserving one candidate at a time allowed an allocator
		// root update to reuse a later candidate and made the eventual release
		// transition internally inconsistent.
		for (const PhysicalSlotId slot: candidateSlots) {
			batch.reserveDataSlot(slot);
		}
		for (CatalogEntry &entry: candidates) {
			allocator_.prepareSetAllocated(batch, entry.physical_slot, false);
		}
		if (!candidateSegmentIds.empty()) catalog_.prepareErase(batch, candidateSegmentIds);
		try {
			// The normal publication path establishes route-frame capacity after
			// excluding every released pool, and reconciles newly created pools.
			publishBatch(batch, writer, options);
		} catch (const BootstrapPublishInterrupted &) {
			// An interruption after the inactive Superblock is durable must also
			// retire the in-memory pool registry; otherwise this process could
			// allocate from a slot the durable Allocator has already released.
			if (bootstrap_.recover() == batch.candidate()) {
				metadata_page_pool_.releaseEmptyCatalogPools(candidateSlots);
			}
			throw;
		}
		metadata_page_pool_.releaseEmptyCatalogPools(candidateSlots);
		return MetadataPagePoolReclamation{batch.candidate(), candidateSlots.size(), std::move(candidateSlots)};
	}

	MetadataPagePoolEvacuation
	RelocatableMetadataStore::evacuateSparseMetadataPagePool(LogSequenceNumber checkpointLsn, uint32_t maxLiveFrames,
															 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot evacuate a metadata page pool");
		}
		if (!checkpointLsn.isValid() || maxLiveFrames == 0U || maxLiveFrames > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata-pool evacuation requires a valid sparse-pool threshold");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);
		if (!current.catalog_root.isValid()) {
			// The bootstrap-only new-file state has no durable pool ownership entry.
			// The first normal publication establishes it before evacuation is allowed.
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}

		std::optional<CatalogEntry> source;
		std::optional<MetadataPagePoolHeader> sourceHeader;
		uint32_t sourceLiveFrames = 0U;
		std::optional<CatalogEntry> pageLocalSource;
		std::optional<MetadataPagePoolHeader> pageLocalSourceHeader;
		uint32_t pageLocalSourceLiveFrames = 0U;
		std::optional<CatalogEntry> mixedRoutedSource;
		std::optional<MetadataPagePoolHeader> mixedRoutedSourceHeader;
		uint32_t mixedRoutedSourceLiveFrames = 0U;
		std::optional<CatalogEntry> routeBranchSource;
		std::optional<MetadataPagePoolHeader> routeBranchSourceHeader;
		uint32_t routeBranchSourceLiveFrames = 0U;
		std::optional<CatalogEntry> bootstrapHeaderSource;
		std::optional<MetadataPagePoolHeader> bootstrapHeaderSourceHeader;
		uint32_t bootstrapHeaderSourceLiveFrames = 0U;
		catalog_.visit(current, [this, &current, maxLiveFrames, &source, &sourceHeader, &sourceLiveFrames,
								 &pageLocalSource, &pageLocalSourceHeader, &pageLocalSourceLiveFrames,
								 &mixedRoutedSource, &mixedRoutedSourceHeader, &mixedRoutedSourceLiveFrames,
								 &routeBranchSource, &routeBranchSourceHeader, &routeBranchSourceLiveFrames,
								 &bootstrapHeaderSource, &bootstrapHeaderSourceHeader,
								 &bootstrapHeaderSourceLiveFrames](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL || entry.state == SegmentState::SGS_OBSOLETE) {
				return;
			}
			const uint8_t invalid = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(entry.owner_id != entry.physical_slot.value()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot)) |
									static_cast<uint8_t>(!metadata_page_pool_.isMetadataPoolSlot(entry.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool evacuation found invalid durable ownership");
			}
			const MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(entry.physical_slot);
			if (header.segment_id != entry.segment_id || header.generation != entry.generation ||
				header.committed_lsn != entry.committed_lsn) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool evacuation found a Catalog/header identity mismatch");
			}
			const uint32_t liveFrames = std::popcount(header.allocated_frames);
			if (liveFrames == 0U || liveFrames > maxLiveFrames) {
				return;
			}
			const auto shouldReplace = [&entry, liveFrames](const std::optional<CatalogEntry> &selected,
															uint32_t selectedFrames) {
				return !selected.has_value() || liveFrames < selectedFrames ||
					   (liveFrames == selectedFrames && entry.physical_slot < selected->physical_slot);
			};
			if (shouldReplace(source, sourceLiveFrames)) {
				source = entry;
				sourceHeader = header;
				sourceLiveFrames = liveFrames;
			}
			if (canRelocateRoutedMetadataPoolPages(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(pageLocalSource, pageLocalSourceLiveFrames)) {
				pageLocalSource = entry;
				pageLocalSourceHeader = header;
				pageLocalSourceLiveFrames = liveFrames;
			}
			if (canEvacuateMixedRoutedMetadataPool(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(mixedRoutedSource, mixedRoutedSourceLiveFrames)) {
				mixedRoutedSource = entry;
				mixedRoutedSourceHeader = header;
				mixedRoutedSourceLiveFrames = liveFrames;
			}
			if (canRefreshMetadataRouteBranchForEvacuation(pages_, current, entry.physical_slot,
														   header.allocated_frames) &&
				shouldReplace(routeBranchSource, routeBranchSourceLiveFrames)) {
				routeBranchSource = entry;
				routeBranchSourceHeader = header;
				routeBranchSourceLiveFrames = liveFrames;
			}
			if (canRefreshBootstrapHeaderForEvacuation(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(bootstrapHeaderSource, bootstrapHeaderSourceLiveFrames)) {
				bootstrapHeaderSource = entry;
				bootstrapHeaderSourceHeader = header;
				bootstrapHeaderSourceLiveFrames = liveFrames;
			}
		});
		if (pageLocalSource.has_value()) {
			source = pageLocalSource;
			sourceHeader = pageLocalSourceHeader;
			sourceLiveFrames = pageLocalSourceLiveFrames;
		} else if (mixedRoutedSource.has_value()) {
			source = mixedRoutedSource;
			sourceHeader = mixedRoutedSourceHeader;
			sourceLiveFrames = mixedRoutedSourceLiveFrames;
		} else if (routeBranchSource.has_value()) {
			source = routeBranchSource;
			sourceHeader = routeBranchSourceHeader;
			sourceLiveFrames = routeBranchSourceLiveFrames;
		} else if (bootstrapHeaderSource.has_value()) {
			source = bootstrapHeaderSource;
			sourceHeader = bootstrapHeaderSourceHeader;
			sourceLiveFrames = bootstrapHeaderSourceLiveFrames;
		}
		if (!source.has_value() || !sourceHeader.has_value()) {
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}
		// A snapshot-pinned reader can retain either stable PageId targets or an
		// old physical route branch in its pinned bootstrap header. The legacy
		// whole-directory clone still relies on mixed physical roots, so keep it blocked.
		const bool hasActiveSnapshots = snapshots_.activeSnapshotCount() != 0U;
		if (hasActiveSnapshots && !pageLocalSource.has_value() && !mixedRoutedSource.has_value() &&
			!routeBranchSource.has_value() && !bootstrapHeaderSource.has_value()) {
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}
		if (hasActiveSnapshots) {
			metadata_page_pool_.protectPoolUntilSnapshotsDrain(source->physical_slot);
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		// Exclude the source before the first COW allocation. In particular the
		// Catalog or Allocator root rewrite must never refill the pool it evicts.
		batch.reserveDataSlot(source->physical_slot);
		const std::optional<uint32_t> pageLocalRelocation = relocateRoutedMetadataPoolPages(
				pages_, batch, current, source->physical_slot, sourceHeader->allocated_frames);
		const std::optional<uint32_t> mixedRoutedRelocation =
				pageLocalRelocation.has_value()
						? std::nullopt
						: evacuateMixedRoutedMetadataPool(pages_, batch, current, source->physical_slot,
														  sourceHeader->allocated_frames);
		const std::optional<uint32_t> routeBranchRefresh =
				pageLocalRelocation.has_value() || mixedRoutedRelocation.has_value()
						? std::nullopt
						: refreshMetadataRouteBranchForEvacuation(pages_, batch, current, source->physical_slot,
																  sourceHeader->allocated_frames);
		const std::optional<uint32_t> bootstrapHeaderRefresh =
				pageLocalRelocation.has_value() || mixedRoutedRelocation.has_value() || routeBranchRefresh.has_value()
						? std::nullopt
						: refreshBootstrapHeaderForEvacuation(pages_, batch, current, source->physical_slot,
															  sourceHeader->allocated_frames);
		if (!pageLocalRelocation.has_value() && !mixedRoutedRelocation.has_value() && !routeBranchRefresh.has_value() &&
			!bootstrapHeaderRefresh.has_value()) {
			// Pre-bootstrap roots and any directory with an unpublishable route
			// identity still use the conservative clone.
			cloneMetadataDirectoryRootsForEvacuation(pages_, batch, current);
		}
		pool_ownership_.preparePublication(batch);
		catalog_.prepareLogicalRootChildren(batch);
		allocator_.prepareLogicalRootChildren(batch);
		locations_.prepareLogicalRootChildren(batch);
		prepareBootstrapRoutes(batch);
		try {
			batch.publish(bootstrap_, options.bootstrap_failpoint);
		} catch (...) {
			// A final durable fence can report an uncertain outcome after its complete
			// candidate Superblock is readable. Reclaim only after this independent
			// recovery check confirms that the new root became current.
			if (bootstrap_.recover() == batch.candidate()) {
				snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
			}
			throw;
		}
		// The evacuation clone can preserve v5 logical children. Bind their
		// resolver to the published route branch before any subsequent read.
		pages_.rebindSuperblock(batch.candidate());
		// Only the durable new root makes the source frames unreachable. This
		// reconciliation is the publication-safe transition to an empty pool;
		// the next reclaimer transaction removes its Catalog ownership and bit.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		const bool sourceEmpty = metadata_page_pool_.isMetadataPoolEmpty(source->physical_slot);
		if (!hasActiveSnapshots && !sourceEmpty) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool evacuation left reachable frames in its source pool");
		}
		if (hasActiveSnapshots && sourceEmpty) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool evacuation reclaimed frames visible to a snapshot");
		}
		return MetadataPagePoolEvacuation{
				batch.candidate(), source->physical_slot,
				pageLocalRelocation.value_or(mixedRoutedRelocation.value_or(
						routeBranchRefresh.value_or(bootstrapHeaderRefresh.value_or(sourceLiveFrames))))};
	}

	MetadataEmergencyReserveRotation
	RelocatableMetadataStore::rotateMetadataEmergencyReserve(LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot rotate the emergency reserve");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const MetadataEmergencyReserveDescriptor previous = current.metadata_emergency_reserve;
		if (!previous.isValid() || metadata_page_pool_.emergencyReserve() != previous) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata reserve descriptor disagrees with the open pool inventory");
		}
		if (!metadata_page_pool_.emergencyReserveHasAllocatedFrames()) {
			return MetadataEmergencyReserveRotation{current, previous, previous, false};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId successorToken = batch.allocateLogicalSegmentId();
		MetadataEmergencyReserveDescriptor successor;
		try {
			const uint64_t fileSize = metadata_page_pool_.physicalFileSize();
			const uint8_t invalidTail =
					static_cast<uint8_t>(fileSize < current.data_region_offset) |
					static_cast<uint8_t>((fileSize - current.data_region_offset) % RSS_SEGMENT_SIZE != 0U) |
					static_cast<uint8_t>(fileSize - current.data_region_offset >
										 (std::numeric_limits<uint64_t>::max() - 1U) / RSS_SEGMENT_SIZE);
			if (invalidTail != 0U) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata emergency reserve has an invalid physical tail");
			}
			const uint64_t tailSlotValue = (fileSize - current.data_region_offset) / RSS_SEGMENT_SIZE;
			PhysicalSlotId successorSlot(tailSlotValue + 1U);
			if (tailSlotValue != 0U) {
				const PhysicalSlotId physicalTail(tailSlotValue);
				// A root-free final slot can only be an already-reclaimed tail or an
				// interrupted unpublished candidate. Reusing it avoids making a
				// permanent single-file hole while never overwriting reachable data.
				if (allocator_.findFirstFree(current, physicalTail, physicalTail).has_value()) {
					successorSlot = physicalTail;
				}
			}
			successor = metadata_page_pool_.rotateEmergencyReserve(successorToken, batch.generation(), checkpointLsn,
																   successorSlot);
			batch.trackUnpublishedPool(successor.physical_slot);
			batch.setMetadataEmergencyReserve(successor);
			// The successor has no Catalog entry by design, but its physical slot is
			// already part of the candidate allocator root before publication.
			allocator_.prepareSetAllocated(batch, successor.physical_slot, true);
			// The former reserve retains its permanent token and is now added as a
			// normal Catalog-owned pool in this same COW generation.
			pool_ownership_.preparePublication(batch);
			prepareBootstrapRoutes(batch);
			batch.publish(bootstrap_, options.bootstrap_failpoint);
		} catch (const BootstrapPublishInterrupted &) {
			if (bootstrap_.recover() != batch.candidate()) {
				metadata_page_pool_.restoreEmergencyReserve(previous, successor);
			} else {
				snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
			}
			throw;
		} catch (...) {
			metadata_page_pool_.restoreEmergencyReserve(previous, successor);
			throw;
		}

		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return MetadataEmergencyReserveRotation{batch.candidate(), previous, successor, true};
	}

	CatalogEntry RelocatableMetadataStore::findCatalog(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata Catalog lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return catalog_.find(snapshot.superblock(), segmentId);
	}

	std::optional<RetireQueueEntry>
	RelocatableMetadataStore::findRetireQueueEntryOptional(const MetadataSnapshotPin &snapshot,
														   PhysicalSlotId slot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findRetireQueueEntryOptional(snapshot.superblock(), slot);
	}

	void RelocatableMetadataStore::visitRetireQueueEntries(
			const MetadataSnapshotPin &snapshot, const std::function<void(const RetireQueueEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Retire Queue traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitRetireQueue(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitCatalogEntries(const MetadataSnapshotPin &snapshot,
													   const std::function<void(const CatalogEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		catalog_.visit(snapshot.superblock(), visitor);
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentIndexPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable index page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_INDEX_PAGE_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentEntityPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_ENTITY_PAGE_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorAdjacencyPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_ADJACENCY_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorRawPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable raw Vector page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_RAW_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findRetiredVectorRawPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable retired Raw Vector page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_RAW_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_RETIRED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorPqPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable PQ page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_PQ_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentLargeValueDataPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared LargeValue data-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL) {
				return;
			}
			if (entry.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue data pool is outside its shared namespace");
			}
			const uint8_t eligible =
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentLargeValueManifestPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared LargeValue Manifest-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
				return;
			}
			if (entry.owner_id != RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue Manifest pool is outside its shared namespace");
			}
			const uint8_t eligible =
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentPropertyPagePools(const MetadataSnapshotPin &snapshot,
														   uint64_t poolOwnerId) const {
		if (!snapshots_.owns(snapshot) || poolOwnerId == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Property page-pool scan requires this store's active snapshot and owner");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools, poolOwnerId](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) &
					static_cast<uint8_t>(entry.owner_id == poolOwnerId) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry> RelocatableMetadataStore::findCurrentStatePagePools(const MetadataSnapshotPin &snapshot,
																				  uint64_t poolOwnerId) const {
		if (!snapshots_.owns(snapshot) || poolOwnerId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page-pool scan requires this store's active snapshot and owner");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools, poolOwnerId](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_STATE_PAGE_POOL) &
					static_cast<uint8_t>(entry.owner_id == poolOwnerId) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	PageLocation RelocatableMetadataStore::findPageLocation(const MetadataSnapshotPin &snapshot, PageId pageId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Page Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findPage(snapshot.superblock(), pageId);
	}

	std::optional<PageLocation> RelocatableMetadataStore::findPageLocationOptional(const MetadataSnapshotPin &snapshot,
																				   PageId pageId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Page Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findPageOptional(snapshot.superblock(), pageId);
	}

	ExtentLocation RelocatableMetadataStore::findExtentLocation(const MetadataSnapshotPin &snapshot,
																ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Extent Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findExtent(snapshot.superblock(), extentId);
	}

	std::optional<ExtentLocation>
	RelocatableMetadataStore::findExtentLocationOptional(const MetadataSnapshotPin &snapshot, ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Extent Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findExtentOptional(snapshot.superblock(), extentId);
	}

	EntityPageLocation RelocatableMetadataStore::findNodePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Node lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findNodePageLocation(snapshot.superblock(), nodeId);
	}

	EntityPageLocation RelocatableMetadataStore::findEdgePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Edge lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEdgePageLocation(snapshot.superblock(), edgeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findNodePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Node lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findNodePageLocationOptional(snapshot.superblock(), nodeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findEdgePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Edge lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEdgePageLocationOptional(snapshot.superblock(), edgeId);
	}

	void RelocatableMetadataStore::findNodePageLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<std::optional<EntityPageLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable Node multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPageLocationsSorted(snapshot.superblock(), MetadataDirectoryType::MDT_NODE_ENTITY,
																			 nodeIds, results, cancellation);
	}

	void RelocatableMetadataStore::findEdgePageLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const std::span<std::optional<EntityPageLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable Edge multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPageLocationsSorted(snapshot.superblock(), MetadataDirectoryType::MDT_EDGE_ENTITY,
																			 edgeIds, results, cancellation);
	}

	void RelocatableMetadataStore::visitNodePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitNodePageLocations(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitEdgePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitEdgePageLocations(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitNodePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitNodePageLocationsUntil(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitEdgePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitEdgePageLocationsUntil(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitNodePageLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterNodeId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPageLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable Entity Map Node traversal "
								  "requires this store's active snapshot, page bound, and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitNodePageLocationsAfter(snapshot.superblock(), afterNodeId, maximumMetadataPages, visitor,
													  visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitExtentLocations(const MetadataSnapshotPin &snapshot,
												   const std::function<void(const ExtentLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Extent directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitExtents(snapshot.superblock(), visitor);
	}

	bool
	RelocatableMetadataStore::visitPageLocationsUntil(const MetadataSnapshotPin &snapshot,
													  const std::function<bool(const PageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Page directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitPagesUntil(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitEntityPropertyLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const EntityPropertyLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Property directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitEntityProperties(snapshot.superblock(), visitor);
	}

	LocationDirectoryRangeCursor RelocatableMetadataStore::openEntityPropertyLocationRangeCursor(
			const MetadataSnapshotPin &snapshot, const uint64_t lowerDirectoryId, const uint64_t upperDirectoryId,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable Entity Property range cursor requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.openRangeCursor(snapshot.superblock(), MetadataDirectoryType::MDT_ENTITY_PROPERTY,
									  lowerDirectoryId, upperDirectoryId, cancellation);
	}

	bool RelocatableMetadataStore::visitEntityPropertyLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterDirectoryId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPropertyLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable resumable Entity Property traversal "
																   "requires this store's active snapshot, page bound, "
																   "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitEntityPropertiesAfter(snapshot.superblock(), afterDirectoryId, maximumMetadataPages,
													 visitor, visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitStateLocations(const MetadataSnapshotPin &snapshot,
												  const std::function<void(const StateLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitStates(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitStateLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterStateId,
															size_t maximumMetadataPages,
															const std::function<bool(const StateLocation &)> &visitor,
															size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable State traversal requires this store's active snapshot, page "
								  "bound, and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitStatesAfter(snapshot.superblock(), afterStateId, maximumMetadataPages, visitor,
										   visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitIndexRoots(const MetadataSnapshotPin &snapshot,
											  const std::function<void(const IndexRootLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Index Root directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitIndexRoots(snapshot.superblock(), visitor);
	}

	void
	RelocatableMetadataStore::visitVectorLocations(const MetadataSnapshotPin &snapshot,
												   const std::function<void(const VectorLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Location Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitVectorLocations(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitVectorManifestLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const VectorManifestLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitVectorManifests(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitVectorManifestLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterIndexId, size_t maximumMetadataPages,
			const std::function<bool(const VectorManifestLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable resumable Vector Manifest traversal "
																   "requires this store's active snapshot, page bound, "
																   "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitVectorManifestsAfter(snapshot.superblock(), afterIndexId, maximumMetadataPages, visitor,
													visitedMetadataPages);
	}

	void RelocatableMetadataStore::visitLargeValueManifestLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const LargeValueManifestLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable LargeValue Manifest Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitLargeValueManifests(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitLargeValueManifestLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterValueId, size_t maximumMetadataPages,
			const std::function<bool(const LargeValueManifestLocation &)> &visitor,
			size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable LargeValue Manifest traversal requires this store's active "
								  "snapshot, page bound, "
								  "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitLargeValueManifestsAfter(snapshot.superblock(), afterValueId, maximumMetadataPages,
														visitor, visitedMetadataPages);
	}

	EntityPropertyLocation RelocatableMetadataStore::findEntityPropertyLocation(const MetadataSnapshotPin &snapshot,
																				const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Property Directory lookup requires a valid key and snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEntityProperty(snapshot.superblock(), key);
	}

	std::optional<EntityPropertyLocation>
	RelocatableMetadataStore::findEntityPropertyLocationOptional(const MetadataSnapshotPin &snapshot,
																 const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable optional Entity Property lookup requires a valid key and snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEntityPropertyOptional(snapshot.superblock(), key);
	}

	void RelocatableMetadataStore::findEntityPropertyLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<std::optional<EntityPropertyLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable Entity Property multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPropertiesSorted(snapshot.superblock(), keys, results, cancellation);
	}

	StateLocation RelocatableMetadataStore::findStateLocation(const MetadataSnapshotPin &snapshot,
															  uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findState(snapshot.superblock(), stateId);
	}

	std::optional<StateLocation>
	RelocatableMetadataStore::findStateLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional State Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findStateOptional(snapshot.superblock(), stateId);
	}

	VectorLocation RelocatableMetadataStore::findVectorLocation(const MetadataSnapshotPin &snapshot,
																uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Location Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorLocation(snapshot.superblock(), vectorId);
	}

	std::optional<VectorLocation>
	RelocatableMetadataStore::findVectorLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Location Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorLocationOptional(snapshot.superblock(), vectorId);
	}

	VectorManifestLocation RelocatableMetadataStore::findVectorManifest(const MetadataSnapshotPin &snapshot,
																		uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorManifest(snapshot.superblock(), indexId);
	}

	std::optional<VectorManifestLocation>
	RelocatableMetadataStore::findVectorManifestOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorManifestOptional(snapshot.superblock(), indexId);
	}

	LargeValueManifestLocation RelocatableMetadataStore::findLargeValueManifest(const MetadataSnapshotPin &snapshot,
																				LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable LargeValue Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findLargeValueManifest(snapshot.superblock(), valueId);
	}

	std::optional<LargeValueManifestLocation>
	RelocatableMetadataStore::findLargeValueManifestOptional(const MetadataSnapshotPin &snapshot,
															 LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable optional LargeValue Manifest Directory "
																   "lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findLargeValueManifestOptional(snapshot.superblock(), valueId);
	}

	IndexRootLocation RelocatableMetadataStore::findIndexRoot(const MetadataSnapshotPin &snapshot,
															  uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Index Root Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findIndexRoot(snapshot.superblock(), indexId);
	}

	std::optional<IndexRootLocation>
	RelocatableMetadataStore::findIndexRootOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Index Root Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findIndexRootOptional(snapshot.superblock(), indexId);
	}

	std::vector<PageLocation> RelocatableMetadataStore::findPageLocationsForSegment(const MetadataSnapshotPin &snapshot,
																					SegmentId segmentId) const {
		if (!snapshots_.owns(snapshot) || !segmentId.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Page directory scan requires this store's active snapshot and a Segment ID");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<PageLocation> locations;
		locations_.visitPages(snapshot.superblock(), [&](const PageLocation &location) {
			if (location.segment_id == segmentId) {
				locations.push_back(location);
			}
		});
		return locations;
	}

	bool RelocatableMetadataStore::isPhysicalSlotAllocated(const MetadataSnapshotPin &snapshot,
														   PhysicalSlotId slot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata free-space lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return allocator_.isAllocated(snapshot.superblock(), slot);
	}

	RelocatableTailTruncationResult RelocatableMetadataStore::truncateReleasedTail() {
		MetadataWriteGuard writer = beginWrite();
		const uint64_t originalBytes = io_->fileSize();
		if (snapshots_.activeSnapshotCount() != 0U) {
			return {RelocatableTailTruncationStatus::VTTS_AWAITING_SNAPSHOTS, originalBytes, originalBytes, 0};
		}
		const Superblock current = bootstrap_.recover();
		if (originalBytes < current.data_region_offset) {
			throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "relocatable tail truncation found a data file shorter than its bootstrap region");
		}
		const uint64_t dataBytes = originalBytes - current.data_region_offset;
		const uint64_t physicalSlots = dataBytes / RSS_SEGMENT_SIZE;
		const auto validateAllocatedSlot = [&](PhysicalSlotId slot, const char *message) {
			const uint8_t invalid = static_cast<uint8_t>(!slot.isValid()) |
									static_cast<uint8_t>(slot.value() > physicalSlots) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE, message);
			}
		};
		catalog_.visit(current, [&](const CatalogEntry &entry) {
			if (entry.state == SegmentState::SGS_SEALED || entry.state == SegmentState::SGS_RETIRED) {
				validateAllocatedSlot(entry.physical_slot,
									  "relocatable tail truncation found a current Catalog slot outside the data file");
			}
		});
		locations_.visitRetireQueue(current, [&](const RetireQueueEntry &entry) {
			validateAllocatedSlot(entry.physical_slot,
								  "relocatable tail truncation found a queued retired slot outside the data file");
		});
		for (const PhysicalSlotId poolSlot: metadata_page_pool_.poolSlots()) {
			validateAllocatedSlot(poolSlot,
								  "relocatable tail truncation found a metadata page pool outside the data file");
		}

		uint64_t retainedSlots = physicalSlots;
		while (retainedSlots != 0U && !allocator_.isAllocated(current, PhysicalSlotId(retainedSlots))) {
			--retainedSlots;
		}
		const uint64_t retainedBytes = current.data_region_offset + retainedSlots * RSS_SEGMENT_SIZE;
		if (retainedBytes >= originalBytes) {
			return {RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE, originalBytes, originalBytes, 0};
		}
		const file_handle_t nativeHandle = io_->getWriteFd();
		if (nativeHandle == INVALID_FILE_HANDLE) {
			return {RelocatableTailTruncationStatus::VTTS_NATIVE_IO_REQUIRED, originalBytes, originalBytes, 0};
		}
		try {
			io_->flushStream();
			// A failed pre-truncate fence guarantees the operating system has not
			// shortened the tail, so callers can safely retry unchanged.
			io_->sync();
			// Keep Buffer Pool tail invalidation and native truncate fault handling in
			// the common StorageIO boundary before the final durability fence.
			io_->truncate(retainedBytes);
		} catch (const std::runtime_error &) {
			return {RelocatableTailTruncationStatus::VTTS_IO_ERROR, originalBytes, originalBytes, 0};
		}
		try {
			io_->sync();
		} catch (const std::runtime_error &) {
			// The truncate already succeeded locally, but its durability is unknown.
			// It is still safe to reopen because this was an already-free suffix.
			return {RelocatableTailTruncationStatus::VTTS_IO_ERROR, originalBytes, retainedBytes, 0};
		}
		return {RelocatableTailTruncationStatus::VTTS_TRUNCATED, originalBytes, retainedBytes,
				physicalSlots - retainedSlots};
	}

	RelocatableCompactionObservation RelocatableMetadataStore::observeCompactionState() const {
		const uint64_t activeSnapshots = snapshots_.activeSnapshotCount();
		MetadataSnapshotPin snapshot = openSnapshot();
		RelocatableCompactionObservation observation;
		observation.generation = snapshot.superblock().generation;
		observation.checkpoint_lsn = snapshot.superblock().checkpoint_lsn;
		observation.active_snapshot_count = activeSnapshots;
		catalog_.visit(snapshot.superblock(), [&observation](const CatalogEntry &entry) {
			observation.sealed_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_SEALED);
			observation.retired_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_RETIRED);
			observation.obsolete_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_OBSOLETE);
		});
		locations_.visitRetireQueue(snapshot.superblock(), [&observation](const RetireQueueEntry &entry) {
			observation.pending_relocation_sources +=
					static_cast<uint64_t>(entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE);
		});
		snapshot.release();
		return observation;
	}

	MetadataPagePoolStats RelocatableMetadataStore::pagePoolStats() const { return metadata_page_pool_.stats(); }

	Superblock RelocatableMetadataStore::migratePageDirectoryToBootstrap(LogSequenceNumber checkpointLsn,
																		 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot migrate the Page Directory");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.usesPageDirectoryBootstrap()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Page Directory bootstrap migration was already published");
		}
		MetadataPageRouteDirectory routes(pages_);
		const size_t requiredFrames = routes.initialBootstrapFrameCount(current);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.reserveMetadataFrames(requiredFrames);
		static_cast<void>(routes.prepareInitialBootstrapMigration(batch));
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		return batch.candidate();
	}

	std::unique_ptr<RelocatableMetadataStore>
	RelocatableMetadataStore::initializeNewComplete(std::shared_ptr<StorageIO> io, const Superblock &initial, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		if (!initial.checkpoint_lsn.isValid() ||
			initial.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable complete initialization requires a non-terminal checkpoint LSN");
		}
		auto metadata = initializeNew(std::move(io), initial, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner));
		static_cast<void>(metadata->convergeInitialBootstrap(LogSequenceNumber(initial.checkpoint_lsn.value() + 1U)));
		return metadata;
	}

	Superblock RelocatableMetadataStore::convergeInitialBootstrap(LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot converge initial bootstrap");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.usesPageDirectoryBootstrap() || current.catalog_root.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable initial bootstrap convergence requires an empty pre-bootstrap Catalog");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		// A capacity preflight may create an empty metadata pool. Reconcile that
		// pool into the candidate Catalog before calculating the final route set;
		// otherwise a newly created file can expose route frames without durable
		// pool ownership after the first later metadata COW.
		pool_ownership_.preparePublication(batch);
		MetadataPageRouteDirectory routes(pages_);
		for (size_t pass = 0U; pass < 8U; ++pass) {
			const size_t requiredFrames = routes.initialBootstrapFrameCount(batch);
			if (batch.ensureMetadataFrameCapacity(requiredFrames) == 0U) {
				break;
			}
			pool_ownership_.preparePublication(batch);
			if (pass == 7U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable initial bootstrap route capacity did not converge");
			}
		}
		batch.reserveMetadataFrames(routes.initialBootstrapFrameCount(batch));
		static_cast<void>(routes.prepareInitialBootstrapMigration(batch));
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		pages_.rebindSuperblock(batch.candidate());
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	uint64_t RelocatableMetadataStore::activeSnapshotCount() const { return snapshots_.activeSnapshotCount(); }


} // namespace graph::storage
