/**
 * @file RelocatableMetadataStore.cpp
 * @brief Serialized operations for the experimental relocatable Catalog metadata stack.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

		[[nodiscard]] bool matchesSharedPagePoolOwnerKind(SegmentKind poolKind, PageOwnerKind ownerKind) {
			switch (poolKind) {
			case SegmentKind::SGK_ENTITY_PAGE_POOL:
				return ownerKind == PageOwnerKind::POK_ENTITY || ownerKind == PageOwnerKind::POK_ENTITY_LABEL;
				case SegmentKind::SGK_PROPERTY_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_PROPERTY;
				case SegmentKind::SGK_INDEX_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_INDEX;
				case SegmentKind::SGK_VECTOR_RAW_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_RAW;
				case SegmentKind::SGK_VECTOR_PQ_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_PQ;
				case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
				case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_VECTOR_MANIFEST;
				case SegmentKind::SGK_STATE_PAGE_POOL:
					return ownerKind == PageOwnerKind::POK_STATE;
				case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_DATA;
				case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
					return ownerKind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
				default:
					return false;
			}
		}

		// This primitive removes only Page Directory routes. Owner kinds that also
		// require Entity, Property, State, or Manifest directory updates must use
		// their dedicated atomic retirement transaction instead.
		[[nodiscard]] bool supportsPageDirectoryOnlyRetirement(PageOwnerKind ownerKind) {
			return ownerKind == PageOwnerKind::POK_INDEX || ownerKind == PageOwnerKind::POK_VECTOR_RAW ||
				   ownerKind == PageOwnerKind::POK_VECTOR_PQ || ownerKind == PageOwnerKind::POK_VECTOR_ADJACENCY;
		}

		[[nodiscard]] bool isEntityPoolPageOwnerKind(PageOwnerKind ownerKind) {
			return ownerKind == PageOwnerKind::POK_ENTITY || ownerKind == PageOwnerKind::POK_ENTITY_LABEL;
		}

		[[nodiscard]] bool hasEntityMapDelta(const EntityPageReplacementRequest &request) {
			return !request.inserted_nodes.empty() || !request.erased_node_ids.empty() ||
				   !request.inserted_edges.empty() || !request.erased_edge_ids.empty();
		}

		[[nodiscard]] bool hasSameNewEntityPoolIdentity(const EntityPageNewPoolBatchRequest &left,
													const EntityPageNewPoolBatchRequest &right) {
			const CatalogSlotReservationRequest &leftReservation = left.reservation;
			const CatalogSlotReservationRequest &rightReservation = right.reservation;
			return leftReservation.segment_id == rightReservation.segment_id &&
				   leftReservation.kind == rightReservation.kind &&
				   leftReservation.owner_id == rightReservation.owner_id &&
				   leftReservation.segment_generation == rightReservation.segment_generation &&
				   leftReservation.state == rightReservation.state &&
				   leftReservation.require_absent == rightReservation.require_absent &&
				   left.first_slot == right.first_slot && left.last_slot == right.last_slot;
		}

		[[nodiscard]] bool hasSameFreshEntityPoolIdentity(const EntityPageFreshPoolDestination &left,
												  const EntityPageFreshPoolDestination &right) {
			return left.segment_id == right.segment_id && left.pool_owner_id == right.pool_owner_id &&
				   left.generation == right.generation && left.first_slot == right.first_slot &&
				   left.last_slot == right.last_slot;
		}

		[[nodiscard]] bool hasSameFreshEntityPoolIdentity(const EntityPageFreshPoolDestination &destination,
														  const EntityPageNewPoolBatchRequest &request) {
			const CatalogSlotReservationRequest &reservation = request.reservation;
			return destination.segment_id == reservation.segment_id &&
				   destination.pool_owner_id == reservation.owner_id &&
				   destination.generation == reservation.segment_generation &&
				   destination.first_slot == request.first_slot && destination.last_slot == request.last_slot &&
				   reservation.kind == SegmentKind::SGK_ENTITY_PAGE_POOL &&
				   reservation.state == SegmentState::SGS_SEALED && reservation.require_absent;
		}

		[[nodiscard]] bool hasSameCatalogEntry(const CatalogEntry &left, const CatalogEntry &right) {
			return left.segment_id == right.segment_id && left.physical_slot == right.physical_slot &&
				   left.kind == right.kind && left.owner_id == right.owner_id && left.generation == right.generation &&
				   left.state == right.state && left.committed_lsn == right.committed_lsn;
		}

	} // namespace

	RelocatableMetadataStore::RelocatableMetadataStore(std::shared_ptr<StorageIO> io, const Superblock &current,
													   bool readOnly) :
		io_(std::move(io)), bootstrap_(io_), pages_(io_, current),
		metadata_page_pool_(io_, current,
							readOnly ? MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY
									 : MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER),
		catalog_(pages_, metadata_page_pool_), locations_(pages_, metadata_page_pool_),
		allocator_(pages_, metadata_page_pool_), pool_ownership_(catalog_, allocator_, metadata_page_pool_, readOnly),
		snapshots_(bootstrap_), retirement_(catalog_, locations_, snapshots_),
		reservation_coordinator_(catalog_, allocator_, locations_),
		segment_retirement_coordinator_(catalog_, allocator_, locations_, retirement_),
		segment_relocation_coordinator_(catalog_, allocator_, locations_, retirement_),
		retire_queue_reclaimer_(catalog_, allocator_, locations_, retirement_), read_only_(readOnly) {
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
																					  const Superblock &initial) {
		initializeRelocatableMetadataFile(io, initial);
		return open(std::move(io));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::open(std::shared_ptr<StorageIO> io) {
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, false));
	}

	std::unique_ptr<RelocatableMetadataStore> RelocatableMetadataStore::openReadOnly(std::shared_ptr<StorageIO> io) {
		const Superblock current = recoverRelocatableMetadataSuperblock(io);
		return std::unique_ptr<RelocatableMetadataStore>(new RelocatableMetadataStore(std::move(io), current, true));
	}

	MetadataSnapshotPin RelocatableMetadataStore::openSnapshot() const { return snapshots_.openSnapshot(); }

	NativeGraphLogicalIdLease RelocatableMetadataStore::reserveNativeGraphLogicalIds(uint64_t nodeCount,
															 uint64_t edgeCount,
															 const MetadataPublishOptions &options) {
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
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
		if (!current.checkpoint_lsn.isValid() || current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max() ||
			(current.next_logical_node_id == 0U && nodeCount != 0U) ||
			(current.next_logical_edge_id == 0U && edgeCount != 0U) ||
			(nodeCount != 0U && nodeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_node_id) ||
			(edgeCount != 0U && edgeCount - 1U > std::numeric_limits<uint64_t>::max() - current.next_logical_edge_id)) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID lease exceeds a durable logical-ID namespace");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		NativeGraphLogicalIdLease lease;
		lease.nodes = {current.next_logical_node_id, nodeCount};
		lease.edges = {current.next_logical_edge_id, edgeCount};
		for (uint64_t ordinal = 0U; ordinal < nodeCount; ++ordinal) {
			static_cast<void>(batch.allocateLogicalNodeId());
		}
		for (uint64_t ordinal = 0U; ordinal < edgeCount; ++ordinal) {
			static_cast<void>(batch.allocateLogicalEdgeId());
		}
		// Leasing changes only durable high-water marks; it must not create a
		// metadata-pool ownership transition or perturb graph-visible roots.
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		if ((nodeCount != 0U && !outstanding_node_id_leases_.emplace(lease.nodes.first_id, lease.nodes.count).second) ||
			(edgeCount != 0U && !outstanding_edge_id_leases_.emplace(lease.edges.first_id, lease.edges.count).second)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph logical-ID lease capability overlaps an outstanding lease");
		}
		lease.superblock = batch.candidate();
		return lease;
	}

	NativeGraphLogicalIdReservation RelocatableMetadataStore::acquireNativeGraphLogicalIds(
			uint64_t nodeCount, uint64_t edgeCount, uint64_t minimumLeaseBlockSize) {
		if (read_only_ || (nodeCount == 0U && edgeCount == 0U) || minimumLeaseBlockSize == 0U ||
			minimumLeaseBlockSize == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
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
		const auto registerOutstanding = [this](const NativeGraphLogicalIdReservation &reservation) {
			const auto registerRanges = [](std::map<uint64_t, uint64_t> &outstanding,
															std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
					if (range.first_id == 0U || range.count == 0U ||
						range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
						!outstanding.emplace(range.first_id, range.count).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
												  "native graph logical-ID acquisition overlaps an outstanding lease");
					}
				}
			};
			registerRanges(outstanding_node_id_leases_, reservation.node_ranges);
			registerRanges(outstanding_edge_id_leases_, reservation.edge_ranges);
		};
		if (remainingNodes == 0U && remainingEdges == 0U) {
			registerOutstanding(result);
			return result;
		}
		const uint64_t requestedNodes = remainingNodes == 0U ? 0U : std::max(remainingNodes, minimumLeaseBlockSize);
		const uint64_t requestedEdges = remainingEdges == 0U ? 0U : std::max(remainingEdges, minimumLeaseBlockSize);
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		validateRequest(requestedNodes, current.next_logical_node_id, "Node");
		validateRequest(requestedEdges, current.next_logical_edge_id, "Edge");
		if (!current.checkpoint_lsn.isValid() || current.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "native graph logical-ID acquisition cannot advance the checkpoint LSN");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		for (uint64_t ordinal = 0U; ordinal < requestedNodes; ++ordinal) {
			static_cast<void>(batch.allocateLogicalNodeId());
		}
		for (uint64_t ordinal = 0U; ordinal < requestedEdges; ++ordinal) {
			static_cast<void>(batch.allocateLogicalEdgeId());
		}
		batch.publish(bootstrap_);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		if (remainingNodes != 0U) {
			available_node_ids_ = {current.next_logical_node_id, requestedNodes};
			take(available_node_ids_, remainingNodes, result.node_ranges);
		}
		if (remainingEdges != 0U) {
			available_edge_ids_ = {current.next_logical_edge_id, requestedEdges};
			take(available_edge_ids_, remainingEdges, result.edge_ranges);
		}
		registerOutstanding(result);
		return result;
	}

	bool RelocatableMetadataStore::claimNativeGraphLogicalIdReservation(
			const NativeGraphLogicalIdReservation &reservation) {
		if (reservation.empty()) {
			return true;
		}
		std::lock_guard leaseLock(logical_id_lease_mutex_);
		const auto validateRanges = [](const std::map<uint64_t, uint64_t> &outstanding,
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
		try {
			std::lock_guard leaseLock(logical_id_lease_mutex_);
			const auto abandonRanges = [](std::map<uint64_t, uint64_t> &outstanding,
														  std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
				for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
					const auto iterator = outstanding.find(range.first_id);
					if (iterator != outstanding.end() && iterator->second == range.count) {
						outstanding.erase(iterator);
					}
				}
			};
			abandonRanges(outstanding_node_id_leases_, reservation.node_ranges);
			abandonRanges(outstanding_edge_id_leases_, reservation.edge_ranges);
		} catch (...) {
			// Rollback is best-effort only for an in-memory capability. Reuse still
			// cannot occur after process restart because durable high-water marks only advance.
		}
	}

	bool RelocatableMetadataStore::ownsSnapshot(const MetadataSnapshotPin &snapshot) const noexcept {
		return snapshots_.owns(snapshot);
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return EntityPropertyPagePoolMaterialization{batch.candidate(), pool, page, location};
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorRawPagesAcrossPools(
			std::span<const VectorRawMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorRawMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable raw Vector materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0;
		for (const VectorRawMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPageCount) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable raw Vector materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);

		std::vector<VectorRawMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorRawMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable raw Vector materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorRawMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable page IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable raw Vector materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		std::vector<VectorLocation> vectorLocations;
		preparer(batches, vectorLocations);

		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::set<uint64_t> uniqueVectorIds;
		std::set<std::pair<PageId, uint16_t>> uniqueVectorSlots;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorRawMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() != pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed Vector materialization
																 // preserves metadata-assigned cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable raw Vector materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation uses one new frame per assigned page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable raw Vector materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		for (const VectorLocation &location: vectorLocations) {
			const bool knownPage = std::ranges::any_of(
					publishedPages, [&location](const PageLocation &page) { return page.page_id == location.page_id; });
			const uint8_t invalidLocation =
					static_cast<uint8_t>(location.vector_id == 0U) | static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
					static_cast<uint8_t>(location.slot_generation == 0U) | static_cast<uint8_t>(!knownPage) |
					static_cast<uint8_t>(current.next_logical_vector_id == 0U) |
					static_cast<uint8_t>(location.vector_id < current.next_logical_vector_id) |
					static_cast<uint8_t>(
							locations_.findVectorLocationOptional(current, location.vector_id).has_value());
			if (invalidLocation != 0U || !uniqueVectorIds.insert(location.vector_id).second ||
				!uniqueVectorSlots.insert({location.page_id, location.slot_id}).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable raw Vector materialization returned an invalid, duplicate, or "
									  "already-published Vector ID");
			}
		}

		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		std::ranges::sort(vectorLocations, {}, &VectorLocation::vector_id);
		for (const VectorLocation &location: vectorLocations) {
			batch.observeLogicalVectorId(location.vector_id);
		}
		locations_.prepareUpsertPages(batch, publishedPages);
		locations_.prepareReplaceVectorLocations(batch, vectorLocations, {});
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorPqPagesAcrossPools(
			std::span<const VectorPqMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorPqMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable PQ Vector materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0U;
		for (const VectorPqMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPageCount) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable PQ Vector materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<VectorPqMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorPqMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable PQ Vector materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_PQ_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorPqMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable PQ Vector materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorPqMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() !=
				pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed preparation preserves cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable PQ Vector materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation assigns one frame per page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable PQ Vector materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		locations_.prepareUpsertPages(batch, publishedPages);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::publishOfflineVectorDerivedRebuild(
			const OfflineVectorDerivedRebuildMetadataMutation &mutation, LogSequenceNumber checkpointLsn,
			const OfflineVectorDerivedRebuildMetadataPreparer &preparer, const MetadataPublishOptions &options) {
		const NativeGraphIndexTreeMutation &mapping = mutation.mapping_tree;
		const NativeGraphIndexTreeMutation &adjacencyMapping = mutation.adjacency_mapping_tree;
		const bool replacesExistingMapping = mutation.expected_mapping_root.has_value();
		const bool hasAdjacency = !mutation.adjacency_pools.empty();
		const bool replacesExistingAdjacencyMapping = mutation.expected_adjacency_mapping_root.has_value();
		const bool retiresDeltaMapping = mutation.expected_delta_mapping_root.has_value();
		const uint8_t malformed =
				static_cast<uint8_t>(mutation.index_id == 0U) | static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(!preparer) |
				static_cast<uint8_t>(!mutation.expected_manifest_pool.segment_id.isValid()) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.owner_id != mutation.index_id) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!mutation.expected_manifest_location.page_id.isValid()) |
				static_cast<uint8_t>(mutation.expected_manifest_location.segment_id !=
									 mutation.expected_manifest_pool.segment_id) |
				static_cast<uint8_t>(mutation.expected_manifest_location.generation !=
									 mutation.expected_manifest_pool.generation) |
				static_cast<uint8_t>(mutation.expected_manifest.index_id != mutation.index_id) |
				static_cast<uint8_t>(mutation.expected_manifest.manifest_page_id !=
									 mutation.expected_manifest_location.page_id) |
				static_cast<uint8_t>(mutation.expected_manifest.generation !=
									 mutation.expected_manifest_location.generation) |
				static_cast<uint8_t>(!mutation.expected_manifest_owner_lsn.isValid()) |
				static_cast<uint8_t>(mutation.codebook_reservations.empty() != mutation.codebook_extents.empty()) |
				static_cast<uint8_t>(!mutation.codebook_reservations.empty() &&
									 (!mutation.codebook_first_slot.isValid() ||
									  !mutation.codebook_last_slot.isValid() ||
									  mutation.codebook_first_slot > mutation.codebook_last_slot)) |
				static_cast<uint8_t>(mutation.pq_pools.empty()) | static_cast<uint8_t>(mapping.index_id == 0U) |
				static_cast<uint8_t>(mapping.index_id != 0U && !VectorPqMapping::isMappingIndexId(mapping.index_id)) |
				static_cast<uint8_t>(mapping.expected_root.has_value()) | static_cast<uint8_t>(mapping.retire_root) |
				static_cast<uint8_t>(!mapping.pool_requests.empty()) |
				static_cast<uint8_t>(mapping.new_pool_requests.empty()) |
				static_cast<uint8_t>(!mapping.replacement_root.has_value()) |
				static_cast<uint8_t>(mapping.replacement_root.has_value() &&
									 !mapping.replacement_root->newly_reserved) |
				static_cast<uint8_t>(mapping.stable_replacement_root.has_value()) |
				static_cast<uint8_t>(replacesExistingMapping &&
									 (mutation.expected_mapping_root->index_id != mapping.index_id ||
									  !mutation.expected_mapping_root->root_page_id.isValid() ||
									  !mutation.expected_mapping_root->root_generation.isValid())) |
				static_cast<uint8_t>(!replacesExistingMapping &&
									 (!mutation.pq_retirements.empty() || !mutation.mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(replacesExistingMapping &&
									 (mutation.pq_retirements.empty() || mutation.mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.index_id == 0U) |
				static_cast<uint8_t>(hasAdjacency &&
									 !VectorAdjacencyMapping::isMappingIndexId(adjacencyMapping.index_id)) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.expected_root.has_value()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.retire_root) |
				static_cast<uint8_t>(hasAdjacency && !adjacencyMapping.pool_requests.empty()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.new_pool_requests.empty()) |
				static_cast<uint8_t>(hasAdjacency && !adjacencyMapping.replacement_root.has_value()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.replacement_root.has_value() &&
									 !adjacencyMapping.replacement_root->newly_reserved) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.stable_replacement_root.has_value()) |
				static_cast<uint8_t>(replacesExistingAdjacencyMapping &&
									 (mutation.expected_adjacency_mapping_root->index_id != adjacencyMapping.index_id ||
									  !mutation.expected_adjacency_mapping_root->root_page_id.isValid() ||
									  !mutation.expected_adjacency_mapping_root->root_generation.isValid())) |
				static_cast<uint8_t>(hasAdjacency && !replacesExistingAdjacencyMapping &&
									 (!mutation.adjacency_retirements.empty() ||
									  !mutation.adjacency_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(replacesExistingAdjacencyMapping &&
									 (mutation.adjacency_retirements.empty() ||
									  mutation.adjacency_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(!hasAdjacency &&
									 (!mutation.adjacency_retirements.empty() || replacesExistingAdjacencyMapping ||
									  !mutation.adjacency_mapping_retirement_pools.empty() ||
									  adjacencyMapping.index_id != 0U || !adjacencyMapping.new_pool_requests.empty())) |
				static_cast<uint8_t>(
						retiresDeltaMapping &&
						(mutation.expected_delta_mapping_root->index_id == 0U ||
						 !mutation.expected_delta_mapping_root->root_page_id.isValid() ||
						 !mutation.expected_delta_mapping_root->root_generation.isValid() ||
						 !VectorDeltaMapping::isMappingIndexId(mutation.expected_delta_mapping_root->index_id) ||
						 mutation.delta_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(!retiresDeltaMapping && !mutation.delta_mapping_retirement_pools.empty());
		if (malformed != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"offline PQ rebuild has an invalid Manifest fence, Codebook candidate, or mapping-tree plan");
		}

		size_t totalPqPages = 0U;
		for (const VectorPqMaterializationPoolRequest &request: mutation.pq_pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPqPages) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid PQ pool placement or page count");
			}
			totalPqPages += request.page_count;
		}
		size_t totalAdjacencyPages = 0U;
		for (const VectorAdjacencyMaterializationPoolRequest &request: mutation.adjacency_pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalAdjacencyPages) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline Vector rebuild has an invalid adjacency pool placement or page count");
			}
			totalAdjacencyPages += request.page_count;
		}
		for (const NewIndexTreePoolRequest &request: mapping.new_pool_requests) {
			const uint8_t invalidPool =
					static_cast<uint8_t>(!request.first_slot.isValid()) |
					static_cast<uint8_t>(!request.last_slot.isValid()) |
					static_cast<uint8_t>(request.first_slot > request.last_slot) |
					static_cast<uint8_t>(request.owner_id != mapping.index_id) |
					static_cast<uint8_t>(request.appended_page_count == 0U) |
					static_cast<uint8_t>(request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid first-use mapping-tree pool request");
			}
		}
		for (const NewIndexTreePoolRequest &request: adjacencyMapping.new_pool_requests) {
			const uint8_t invalidPool =
					static_cast<uint8_t>(!request.first_slot.isValid()) |
					static_cast<uint8_t>(!request.last_slot.isValid()) |
					static_cast<uint8_t>(request.first_slot > request.last_slot) |
					static_cast<uint8_t>(request.owner_id != adjacencyMapping.index_id) |
					static_cast<uint8_t>(request.appended_page_count == 0U) |
					static_cast<uint8_t>(request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline Vector rebuild has an invalid adjacency mapping-tree pool request");
			}
		}
		if (mapping.replacement_root->pool_ordinal >= mapping.new_pool_requests.size() ||
			mapping.replacement_root->appended_page_ordinal >=
					mapping.new_pool_requests[mapping.replacement_root->pool_ordinal].appended_page_count) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "offline PQ rebuild mapping-tree root does not name a candidate PageId");
		}
		if (hasAdjacency &&
			(adjacencyMapping.replacement_root->pool_ordinal >= adjacencyMapping.new_pool_requests.size() ||
			 adjacencyMapping.replacement_root->appended_page_ordinal >=
					 adjacencyMapping.new_pool_requests[adjacencyMapping.replacement_root->pool_ordinal]
							 .appended_page_count)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"offline Vector rebuild adjacency mapping-tree root does not name a candidate PageId");
		}

		uint64_t previousPqRetirementPageId = 0U;
		std::set<uint64_t> retiredPageIds;
		for (const PageRetirement &retirement: mutation.pq_retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousPqRetirementPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id != mutation.index_id) |
					static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_PQ) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid or non-canonical retired PQ source page");
			}
			previousPqRetirementPageId = location.page_id.value();
		}
		uint64_t previousAdjacencyRetirementPageId = 0U;
		for (const PageRetirement &retirement: mutation.adjacency_retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousAdjacencyRetirementPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id != (mutation.index_id | (UINT64_C(1) << 63U))) |
					static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_ADJACENCY) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"offline Vector rebuild has an invalid or non-canonical retired adjacency source page");
			}
			previousAdjacencyRetirementPageId = location.page_id.value();
		}

		uint64_t previousRetirementSegmentId = 0U;
		bool retiresExpectedMappingRoot = false;
		for (const IndexTreeMutationPoolRequest &request: mutation.mapping_retirement_pools) {
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.segment_id.value() <= previousRetirementSegmentId) |
					static_cast<uint8_t>(!request.expected_replacements.empty()) |
					static_cast<uint8_t>(request.appended_page_count != 0U) |
					static_cast<uint8_t>(request.expected_retirements.empty());
			if (invalidRequest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid or unordered mapping-tree retirement pool");
			}
			uint64_t previousRetirementPageId = 0U;
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation &location = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(!location.page_id.isValid()) |
						static_cast<uint8_t>(location.page_id.value() <= previousRetirementPageId) |
						static_cast<uint8_t>(location.segment_id != request.segment_id) |
						static_cast<uint8_t>(!location.generation.isValid()) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
						static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
				if (invalidRetirement != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "offline PQ rebuild has an invalid, duplicate, or non-canonical mapping-tree "
										  "retirement page");
				}
				if (replacesExistingMapping && location.page_id == mutation.expected_mapping_root->root_page_id &&
					location.generation == mutation.expected_mapping_root->root_generation) {
					retiresExpectedMappingRoot = true;
				}
				previousRetirementPageId = location.page_id.value();
			}
			previousRetirementSegmentId = request.segment_id.value();
		}
		if (replacesExistingMapping && !retiresExpectedMappingRoot) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "offline PQ rebuild replacement does not retire its expected mapping-tree root");
		}
		const auto validateAdditionalMappingRetirements =
				[&retiredPageIds](std::span<const IndexTreeMutationPoolRequest> requests,
								  const std::optional<IndexRootLocation> &expectedRoot, const char *message) {
					uint64_t previousSegmentId = 0U;
					bool retiresExpectedRoot = !expectedRoot.has_value();
					for (const IndexTreeMutationPoolRequest &request: requests) {
						const uint8_t invalidRequest =
								static_cast<uint8_t>(!request.segment_id.isValid()) |
								static_cast<uint8_t>(request.segment_id.value() <= previousSegmentId) |
								static_cast<uint8_t>(!request.expected_replacements.empty()) |
								static_cast<uint8_t>(request.appended_page_count != 0U) |
								static_cast<uint8_t>(request.expected_retirements.empty());
						if (invalidRequest != 0U) {
							throw FormatException(FormatError::FERR_INVALID_HEADER, message);
						}
						uint64_t previousPageId = 0U;
						for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
							const PageLocation &location = retirement.location;
							const uint8_t invalidRetirement =
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(location.segment_id != request.segment_id) |
									static_cast<uint8_t>(!location.generation.isValid()) |
									static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
									static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
							if (invalidRetirement != 0U) {
								throw FormatException(FormatError::FERR_INVALID_HEADER, message);
							}
							if (expectedRoot.has_value() && location.page_id == expectedRoot->root_page_id &&
								location.generation == expectedRoot->root_generation) {
								retiresExpectedRoot = true;
							}
							previousPageId = location.page_id.value();
						}
						previousSegmentId = request.segment_id.value();
					}
					if (!retiresExpectedRoot) {
						throw FormatException(FormatError::FERR_INVALID_HEADER, message);
					}
				};
		if (hasAdjacency) {
			validateAdditionalMappingRetirements(
					mutation.adjacency_mapping_retirement_pools, mutation.expected_adjacency_mapping_root,
					"offline Vector rebuild does not retire its expected adjacency mapping-tree root");
		}
		if (retiresDeltaMapping) {
			validateAdditionalMappingRetirements(
					mutation.delta_mapping_retirement_pools, mutation.expected_delta_mapping_root,
					"offline Vector rebuild does not retire its expected delta mapping-tree root");
		}

		std::set<uint64_t> codebookSegmentIds;
		std::set<uint64_t> codebookExtentIds;
		for (size_t index = 0U; index < mutation.codebook_reservations.size(); ++index) {
			const CatalogSlotReservationRequest &request = mutation.codebook_reservations[index];
			const ExtentLocation &extent = mutation.codebook_extents[index];
			const uint8_t invalidCodebook =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(request.owner_id == 0U) |
					static_cast<uint8_t>(!request.segment_generation.isValid()) |
					static_cast<uint8_t>(request.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!request.require_absent) |
					static_cast<uint8_t>(extent.segment_id != request.segment_id) |
					static_cast<uint8_t>(extent.generation != request.segment_generation) |
					static_cast<uint8_t>(!extent.extent_id.isValid()) |
					static_cast<uint8_t>(extent.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(extent.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!codebookSegmentIds.insert(request.segment_id.value()).second) |
					static_cast<uint8_t>(!codebookExtentIds.insert(extent.extent_id.value()).second);
			if (invalidCodebook != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"offline PQ rebuild has duplicate or malformed immutable Codebook extent identities");
			}
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const VectorManifestLocation visibleManifest = locations_.findVectorManifest(current, mutation.index_id);
		const PageLocation visibleManifestPage =
				locations_.findPage(current, mutation.expected_manifest_location.page_id);
		const CatalogEntry visibleManifestPool = catalog_.find(current, visibleManifestPage.segment_id);
		const std::optional<IndexRootLocation> visibleMappingRoot =
				locations_.findIndexRootOptional(current, mapping.index_id);
		const std::optional<IndexRootLocation> visibleAdjacencyMappingRoot =
				hasAdjacency ? locations_.findIndexRootOptional(current, adjacencyMapping.index_id) : std::nullopt;
		const std::optional<IndexRootLocation> visibleDeltaMappingRoot =
				retiresDeltaMapping
						? locations_.findIndexRootOptional(current, mutation.expected_delta_mapping_root->index_id)
						: std::nullopt;
		const uint8_t stale =
				static_cast<uint8_t>(visibleManifest != mutation.expected_manifest) |
				static_cast<uint8_t>(visibleManifestPage != mutation.expected_manifest_location) |
				static_cast<uint8_t>(!hasSameCatalogEntry(visibleManifestPool, mutation.expected_manifest_pool)) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visibleManifestPool.physical_slot)) |
				static_cast<uint8_t>(
						replacesExistingMapping &&
						(!visibleMappingRoot.has_value() || *visibleMappingRoot != *mutation.expected_mapping_root)) |
				static_cast<uint8_t>(!replacesExistingMapping && visibleMappingRoot.has_value());
		const uint8_t derivedStale =
				static_cast<uint8_t>(hasAdjacency && replacesExistingAdjacencyMapping &&
									 (!visibleAdjacencyMappingRoot.has_value() ||
									  *visibleAdjacencyMappingRoot != *mutation.expected_adjacency_mapping_root)) |
				static_cast<uint8_t>(hasAdjacency && !replacesExistingAdjacencyMapping &&
									 visibleAdjacencyMappingRoot.has_value()) |
				static_cast<uint8_t>(retiresDeltaMapping &&
									 (!visibleDeltaMappingRoot.has_value() ||
									  *visibleDeltaMappingRoot != *mutation.expected_delta_mapping_root));
		if (stale != 0U || derivedStale != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild has a stale Manifest fence or mapping-root transition");
		}

		std::map<uint64_t, ExistingPoolPageRetirementBatch> pqRetirementBatchesBySegment;
		for (const PageRetirement &retirement: mutation.pq_retirements) {
			const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
										   static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_PQ_POOL) |
										   static_cast<uint8_t>(pool.owner_id != mutation.index_id) |
										   static_cast<uint8_t>(pool.generation != retirement.location.generation) |
										   static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										   static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has a stale, foreign, or unavailable retired PQ source page");
			}
			auto [iterator, inserted] = pqRetirementBatchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}
		std::map<uint64_t, ExistingPoolPageRetirementBatch> adjacencyRetirementBatchesBySegment;
		for (const PageRetirement &retirement: mutation.adjacency_retirements) {
			const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
										   static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
										   static_cast<uint8_t>(pool.owner_id != mutation.index_id) |
										   static_cast<uint8_t>(pool.generation != retirement.location.generation) |
										   static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										   static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild has a stale, foreign, or unavailable retired adjacency source page");
			}
			auto [iterator, inserted] = adjacencyRetirementBatchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}

		std::vector<IndexTreeMutationPoolBatch> mappingRetirementBatches;
		mappingRetirementBatches.reserve(mutation.mapping_retirement_pools.size());
		for (const IndexTreeMutationPoolRequest &request: mutation.mapping_retirement_pools) {
			const CatalogEntry pool = catalog_.find(current, request.segment_id);
			// Existing Index pools can be shared by several logical mapping trees.
			// The immutable retirement plan and page headers authenticate the tree;
			// the Catalog owner identifies only the allocation namespace.
			const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
													static_cast<uint8_t>(pool.owner_id == 0U) |
										static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild requires current owned mapping-tree retirement pools");
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
				const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
											   static_cast<uint8_t>(retirement.location.generation != pool.generation);
				if (invalidCurrent != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild has a stale or foreign mapping-tree retirement page");
				}
			}
			mappingRetirementBatches.push_back({pool, {}, {}, {}, {}, false, request.expected_retirements});
		}
		const auto materializeTreeRetirementBatches = [this,
													   &current](std::span<const IndexTreeMutationPoolRequest> requests,
																 uint64_t expectedIndexId, const char *message) {
			std::vector<IndexTreeMutationPoolBatch> result;
			result.reserve(requests.size());
			for (const IndexTreeMutationPoolRequest &request: requests) {
				const CatalogEntry pool = catalog_.find(current, request.segment_id);
					// `expectedIndexId` identifies the B+Tree pages, not the shared
					// allocation namespace that owns this Index pool.
					const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
													static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
					const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
					if (visible != retirement.location || retirement.location.generation != pool.generation) {
						throw FormatException(FormatError::FERR_INVALID_STATE, message);
					}
				}
				result.push_back({pool, {}, {}, {}, {}, false, request.expected_retirements});
			}
			return result;
		};
		std::vector<IndexTreeMutationPoolBatch> adjacencyMappingRetirementBatches;
		if (hasAdjacency) {
			adjacencyMappingRetirementBatches = materializeTreeRetirementBatches(
					mutation.adjacency_mapping_retirement_pools, adjacencyMapping.index_id,
					"offline Vector rebuild requires current owned adjacency mapping-tree retirement pools");
		}
		std::vector<IndexTreeMutationPoolBatch> deltaMappingRetirementBatches;
		if (retiresDeltaMapping) {
			deltaMappingRetirementBatches = materializeTreeRetirementBatches(
					mutation.delta_mapping_retirement_pools, mutation.expected_delta_mapping_root->index_id,
					"offline Vector rebuild requires current owned delta mapping-tree retirement pools");
		}
		for (size_t index = 0U; index < mutation.codebook_reservations.size(); ++index) {
			if (catalog_.findOptional(current, mutation.codebook_reservations[index].segment_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline PQ rebuild Codebook candidate reuses a reachable Catalog segment identity");
			}
			if (locations_.findExtentOptional(current, mutation.codebook_extents[index].extent_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild Codebook candidate reuses a reachable Extent identity");
			}
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		OfflineVectorDerivedRebuildMetadataMaterialization materialization;
		materialization.codebook_extents.reserve(mutation.codebook_reservations.size());
		materialization.pq_retirement_pools.reserve(pqRetirementBatchesBySegment.size());
		for (auto &[segmentId, retirementBatch]: pqRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			materialization.pq_retirement_pools.push_back(std::move(retirementBatch));
		}
		materialization.adjacency_retirement_pools.reserve(adjacencyRetirementBatchesBySegment.size());
		for (auto &[segmentId, retirementBatch]: adjacencyRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			materialization.adjacency_retirement_pools.push_back(std::move(retirementBatch));
		}
		materialization.mapping_retirement_pools = std::move(mappingRetirementBatches);
		materialization.adjacency_mapping_retirement_pools = std::move(adjacencyMappingRetirementBatches);
		materialization.delta_mapping_retirement_pools = std::move(deltaMappingRetirementBatches);
		std::vector<CatalogEntry> catalogEntries;
		catalogEntries.reserve(mutation.codebook_reservations.size() + mutation.pq_pools.size() +
							   mapping.new_pool_requests.size() + mutation.adjacency_pools.size() +
							   adjacencyMapping.new_pool_requests.size());
		for (const CatalogSlotReservationRequest &request: mutation.codebook_reservations) {
			const std::optional<PhysicalSlotId> slot = allocator_.prepareReserveFirstFree(
					batch, mutation.codebook_first_slot, mutation.codebook_last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for an immutable Codebook extent");
			}
			CatalogEntry entry{request.segment_id,		   *slot,		  request.kind, request.owner_id,
							   request.segment_generation, request.state, checkpointLsn};
			batch.observeLogicalSegmentId(entry.segment_id);
			materialization.codebook_extents.push_back(entry);
			catalogEntries.push_back(entry);
		}

		materialization.pq_pools.reserve(mutation.pq_pools.size());
		for (const VectorPqMaterializationPoolRequest &request: mutation.pq_pools) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for a PQ Vector page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_VECTOR_PQ_POOL,
							  mutation.index_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			materialization.pq_pools.push_back({pool, {}, {}});
			catalogEntries.push_back(pool);
		}
		materialization.adjacency_pools.reserve(mutation.adjacency_pools.size());
		for (const VectorAdjacencyMaterializationPoolRequest &request: mutation.adjacency_pools) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline Vector rebuild has no free physical slot for an adjacency page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_VECTOR_ADJACENCY_POOL,
							  mutation.index_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			materialization.adjacency_pools.push_back({pool, {}, {}});
			catalogEntries.push_back(pool);
		}

		materialization.mapping_tree_pools.reserve(mapping.new_pool_requests.size());
		for (const NewIndexTreePoolRequest &request: mapping.new_pool_requests) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for a PQ mapping-tree page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_INDEX_PAGE_POOL,
							  request.owner_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			IndexTreeMutationPoolBatch treePool;
			treePool.pool = pool;
			treePool.newly_reserved = true;
			treePool.appended_page_ids.reserve(request.appended_page_count);
			treePool.appended_locations.reserve(request.appended_page_count);
			materialization.mapping_tree_pools.push_back(std::move(treePool));
			catalogEntries.push_back(pool);
		}
		materialization.adjacency_mapping_tree_pools.reserve(adjacencyMapping.new_pool_requests.size());
		for (const NewIndexTreePoolRequest &request: adjacencyMapping.new_pool_requests) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild has no free physical slot for an adjacency mapping-tree page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_INDEX_PAGE_POOL,
							  request.owner_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			IndexTreeMutationPoolBatch treePool;
			treePool.pool = pool;
			treePool.newly_reserved = true;
			treePool.appended_page_ids.reserve(request.appended_page_count);
			treePool.appended_locations.reserve(request.appended_page_count);
			materialization.adjacency_mapping_tree_pools.push_back(std::move(treePool));
			catalogEntries.push_back(pool);
		}
		for (VectorPqMaterializationPoolBatch &pool: materialization.pq_pools) {
			const auto request = mutation.pq_pools[&pool - materialization.pq_pools.data()];
			pool.page_ids.reserve(request.page_count);
			pool.locations.reserve(request.page_count);
			for (size_t pageOrdinal = 0U; pageOrdinal < request.page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild allocated an already reachable PQ PageId");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_pools.size(); ++poolOrdinal) {
			VectorAdjacencyMaterializationPoolBatch &pool = materialization.adjacency_pools[poolOrdinal];
			const VectorAdjacencyMaterializationPoolRequest &request = mutation.adjacency_pools[poolOrdinal];
			pool.page_ids.reserve(request.page_count);
			pool.locations.reserve(request.page_count);
			for (size_t pageOrdinal = 0U; pageOrdinal < request.page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline Vector rebuild allocated an already reachable adjacency PageId");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.mapping_tree_pools.size(); ++poolOrdinal) {
			IndexTreeMutationPoolBatch &pool = materialization.mapping_tree_pools[poolOrdinal];
			const NewIndexTreePoolRequest &request = mapping.new_pool_requests[poolOrdinal];
			for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild allocated an already reachable mapping-tree PageId");
				}
				pool.appended_page_ids.push_back(pageId);
				pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_mapping_tree_pools.size();
			 ++poolOrdinal) {
			IndexTreeMutationPoolBatch &pool = materialization.adjacency_mapping_tree_pools[poolOrdinal];
			const NewIndexTreePoolRequest &request = adjacencyMapping.new_pool_requests[poolOrdinal];
			for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"offline Vector rebuild allocated an already reachable adjacency mapping PageId");
				}
				pool.appended_page_ids.push_back(pageId);
				pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		const IndexTreeMutationPoolBatch &rootPool =
				materialization.mapping_tree_pools[mapping.replacement_root->pool_ordinal];
		materialization.mapping_root = {mapping.index_id,
										rootPool.appended_page_ids[mapping.replacement_root->appended_page_ordinal],
										rootPool.pool.generation};
		if (hasAdjacency) {
			const IndexTreeMutationPoolBatch &adjacencyRootPool =
					materialization.adjacency_mapping_tree_pools[adjacencyMapping.replacement_root->pool_ordinal];
			materialization.adjacency_mapping_root = {
					adjacencyMapping.index_id,
					adjacencyRootPool.appended_page_ids[adjacencyMapping.replacement_root->appended_page_ordinal],
					adjacencyRootPool.pool.generation};
		}
		materialization.manifest_replacement = {mutation.expected_manifest_location.page_id,
												mutation.expected_manifest_pool.segment_id, 0U,
												mutation.expected_manifest_pool.generation, 0U};
		std::ranges::sort(catalogEntries, {}, &CatalogEntry::segment_id);
		catalog_.prepareUpsert(batch, catalogEntries);

		const OfflineVectorDerivedRebuildMetadataMaterialization planned = materialization;
		preparer(materialization);
		const bool changedCodebookEntries =
				materialization.codebook_extents.size() != planned.codebook_extents.size() ||
				!std::ranges::equal(materialization.codebook_extents, planned.codebook_extents, hasSameCatalogEntry);
		const auto hasSamePqRetirementBatches = [](std::span<const ExistingPoolPageRetirementBatch> actual,
												   std::span<const ExistingPoolPageRetirementBatch> expected) {
			if (actual.size() != expected.size()) {
				return false;
			}
			for (size_t ordinal = 0U; ordinal < actual.size(); ++ordinal) {
				if (!hasSameCatalogEntry(actual[ordinal].pool, expected[ordinal].pool) ||
					actual[ordinal].retirements.size() != expected[ordinal].retirements.size()) {
					return false;
				}
				for (size_t retirementOrdinal = 0U; retirementOrdinal < actual[ordinal].retirements.size();
					 ++retirementOrdinal) {
					const PageRetirement &left = actual[ordinal].retirements[retirementOrdinal];
					const PageRetirement &right = expected[ordinal].retirements[retirementOrdinal];
					if (left.location != right.location || left.expected_owner_id != right.expected_owner_id ||
						left.expected_owner_kind != right.expected_owner_kind ||
						left.expected_owner_lsn != right.expected_owner_lsn) {
						return false;
					}
				}
			}
			return true;
		};
		const auto hasSameMappingRetirementBatches = [](std::span<const IndexTreeMutationPoolBatch> actual,
														std::span<const IndexTreeMutationPoolBatch> expected) {
			if (actual.size() != expected.size()) {
				return false;
			}
			for (size_t ordinal = 0U; ordinal < actual.size(); ++ordinal) {
				const IndexTreeMutationPoolBatch &left = actual[ordinal];
				const IndexTreeMutationPoolBatch &right = expected[ordinal];
				if (!hasSameCatalogEntry(left.pool, right.pool) || !left.expected_replacements.empty() ||
					!left.appended_page_ids.empty() || left.newly_reserved != right.newly_reserved ||
					left.expected_retirements != right.expected_retirements || !left.replacement_locations.empty() ||
					!left.appended_locations.empty()) {
					return false;
				}
			}
			return true;
		};
		if (changedCodebookEntries) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned Codebook identity");
		}
		if (materialization.mapping_root != planned.mapping_root ||
			materialization.pq_pools.size() != planned.pq_pools.size() ||
			materialization.mapping_tree_pools.size() != planned.mapping_tree_pools.size() ||
			materialization.adjacency_mapping_root != planned.adjacency_mapping_root ||
			materialization.adjacency_pools.size() != planned.adjacency_pools.size() ||
			materialization.adjacency_mapping_tree_pools.size() != planned.adjacency_mapping_tree_pools.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned mapping or pool identity");
		}
		if (!hasSamePqRetirementBatches(materialization.pq_retirement_pools, planned.pq_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned PQ retirement plan");
		}
		if (!hasSamePqRetirementBatches(materialization.adjacency_retirement_pools,
										planned.adjacency_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline Vector rebuild preparer changed a metadata-owned adjacency retirement plan");
		}
		if (!hasSameMappingRetirementBatches(materialization.mapping_retirement_pools,
											 planned.mapping_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned mapping-tree retirement plan");
		}
		if (!hasSameMappingRetirementBatches(materialization.adjacency_mapping_retirement_pools,
											 planned.adjacency_mapping_retirement_pools) ||
			!hasSameMappingRetirementBatches(materialization.delta_mapping_retirement_pools,
											 planned.delta_mapping_retirement_pools)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"offline Vector rebuild preparer changed a metadata-owned derived mapping-tree retirement plan");
		}

		size_t mappingPageCount = 0U;
		for (const IndexTreeMutationPoolBatch &pool: materialization.mapping_tree_pools) {
			mappingPageCount += pool.appended_page_ids.size();
		}
		size_t adjacencyMappingPageCount = 0U;
		for (const IndexTreeMutationPoolBatch &pool: materialization.adjacency_mapping_tree_pools) {
			adjacencyMappingPageCount += pool.appended_page_ids.size();
		}
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPqPages + totalAdjacencyPages + mappingPageCount + adjacencyMappingPageCount + 1U);
		std::set<uint64_t> pageIds;
		std::set<std::pair<uint64_t, uint16_t>> frames;
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.pq_pools.size(); ++poolOrdinal) {
			const VectorPqMaterializationPoolBatch &actual = materialization.pq_pools[poolOrdinal];
			const VectorPqMaterializationPoolBatch &expected = planned.pq_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.page_ids != expected.page_ids ||
				actual.locations.size() != expected.locations.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild preparer changed a PQ pool identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U || retiredPageIds.contains(location.page_id.value()) ||
					!pageIds.insert(location.page_id.value()).second ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild preparer returned an invalid or duplicate PQ page frame");
				}
				publishedPages.push_back(location);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_pools.size(); ++poolOrdinal) {
			const VectorAdjacencyMaterializationPoolBatch &actual = materialization.adjacency_pools[poolOrdinal];
			const VectorAdjacencyMaterializationPoolBatch &expected = planned.adjacency_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.page_ids != expected.page_ids ||
				actual.locations.size() != expected.locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild preparer changed an adjacency pool identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U || retiredPageIds.contains(location.page_id.value()) ||
					!pageIds.insert(location.page_id.value()).second ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"offline Vector rebuild preparer returned an invalid or duplicate adjacency frame");
				}
				publishedPages.push_back(location);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.mapping_tree_pools.size(); ++poolOrdinal) {
			const IndexTreeMutationPoolBatch &actual = materialization.mapping_tree_pools[poolOrdinal];
			const IndexTreeMutationPoolBatch &expected = planned.mapping_tree_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.newly_reserved != expected.newly_reserved ||
				actual.appended_page_ids != expected.appended_page_ids ||
				actual.appended_locations.size() != expected.appended_locations.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild preparer changed a mapping-tree identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.appended_locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.appended_page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U || retiredPageIds.contains(location.page_id.value()) ||
					!pageIds.insert(location.page_id.value()).second ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"offline PQ rebuild preparer returned an invalid or duplicate mapping-tree frame");
				}
				publishedPages.push_back(location);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_mapping_tree_pools.size();
			 ++poolOrdinal) {
			const IndexTreeMutationPoolBatch &actual = materialization.adjacency_mapping_tree_pools[poolOrdinal];
			const IndexTreeMutationPoolBatch &expected = planned.adjacency_mapping_tree_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.newly_reserved != expected.newly_reserved ||
				actual.appended_page_ids != expected.appended_page_ids ||
				actual.appended_locations.size() != expected.appended_locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild preparer changed an adjacency mapping-tree identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.appended_locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.appended_page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U || retiredPageIds.contains(location.page_id.value()) ||
					!pageIds.insert(location.page_id.value()).second ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline Vector rebuild preparer returned an invalid or duplicate adjacency "
										  "mapping-tree frame");
				}
				publishedPages.push_back(location);
			}
		}
		const PageLocation &manifest = materialization.manifest_replacement;
		const uint8_t invalidManifest =
				static_cast<uint8_t>(manifest.page_id != mutation.expected_manifest_location.page_id) |
				static_cast<uint8_t>(manifest.segment_id != mutation.expected_manifest_pool.segment_id) |
				static_cast<uint8_t>(manifest.generation != mutation.expected_manifest_pool.generation) |
				static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(manifest.page_index == mutation.expected_manifest_location.page_index) |
				static_cast<uint8_t>(manifest.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		if (invalidManifest != 0U || retiredPageIds.contains(manifest.page_id.value()) ||
			!pageIds.insert(manifest.page_id.value()).second ||
			!frames.emplace(manifest.segment_id.value(), manifest.page_index).second) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer returned an invalid Manifest replacement frame");
		}
		publishedPages.push_back(manifest);
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		const std::vector<uint64_t> erasedPageIds(retiredPageIds.begin(), retiredPageIds.end());
		locations_.prepareReplacePages(batch, publishedPages, erasedPageIds);
		if (!mutation.codebook_extents.empty()) {
			locations_.prepareUpsertExtents(batch, mutation.codebook_extents);
		}
		std::array<IndexRootLocation, 2U> replacementRoots{materialization.mapping_root,
														   materialization.adjacency_mapping_root};
		const std::span<const IndexRootLocation> publishedRoots(replacementRoots.data(), hasAdjacency ? 2U : 1U);
		const std::array<uint64_t, 1U> erasedDeltaRoot{
				{retiresDeltaMapping ? mutation.expected_delta_mapping_root->index_id : 0U}};
		const std::span<const uint64_t> erasedRoots =
				retiresDeltaMapping ? std::span<const uint64_t>(erasedDeltaRoot) : std::span<const uint64_t>{};
		locations_.prepareReplaceIndexRoots(batch, publishedRoots, erasedRoots);
		const VectorManifestLocation replacementManifest{mutation.index_id, manifest.page_id, manifest.generation};
		locations_.prepareReplaceVectorManifests(batch, std::span(&replacementManifest, 1U), {});
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorAdjacencyPagesAcrossPools(
			std::span<const VectorAdjacencyMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorAdjacencyMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable adjacency materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0U;
		for (const VectorAdjacencyMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPageCount) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable adjacency materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<VectorAdjacencyMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorAdjacencyMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable adjacency materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorAdjacencyMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable adjacency materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorAdjacencyMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() !=
				pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed preparation preserves cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable adjacency materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation assigns one unique frame per page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable adjacency materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		locations_.prepareUpsertPages(batch, publishedPages);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorManifestPage(uint64_t indexId, PhysicalSlotId firstSlot,
																		  PhysicalSlotId lastSlot,
																		  LogSequenceNumber checkpointLsn,
																		  const NewVectorManifestPagePreparer &preparer,
																		  const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(!firstSlot.isValid()) |
									 static_cast<uint8_t>(!lastSlot.isValid()) |
									 static_cast<uint8_t>(firstSlot > lastSlot) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest materialization requires an index, placement, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest materialization has no free physical slot in its requested range");
		}
		CatalogEntry pool;
		pool.segment_id = batch.allocateLogicalSegmentId();
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		pool.owner_id = indexId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		catalog_.prepareUpsert(batch, std::span(&pool, 1));

		const PageId pageId = batch.allocateLogicalPageId();
		if (locations_.findPageOptional(current, pageId)
					.has_value()) { // ZYX_COV_EXCL_LINE: logical page IDs are never reused.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest materialization allocated an already reachable Page ID");
		}
		PageLocation location{pageId, pool.segment_id, 0, pool.generation, 0};
		preparer(pool, pageId, location);
		const uint8_t invalidLocation = static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.segment_id != pool.segment_id) |
										static_cast<uint8_t>(location.generation != pool.generation) |
										static_cast<uint8_t>(location.page_index != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(location.free_bytes != 0U);
		if (invalidLocation != 0U) { // ZYX_COV_EXCL_LINE: the typed writer prepares exactly this one dedicated frame.
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest materialization preparer returned an invalid Page Directory location");
		}
		const VectorManifestLocation manifest{indexId, pageId, pool.generation};
		locations_.prepareUpsertPages(batch, std::span(&location, 1));
		locations_.prepareReplaceVectorManifests(batch, std::span(&manifest, 1), {});
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
										 request.expected_retirements.empty());
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
			batches.push_back({pool,
							   request.expected_replacements,
							   {},
							   request.expected_replacements,
							   {},
							   false,
							   request.expected_retirements});
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
			batches.push_back({pool, {}, {}, {}, {}, true, {}});
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
			auto validatePublishedLocation = [&](const PageLocation &location, PageId expectedPageId) {
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expectedPageId) |
						static_cast<uint8_t>(location.segment_id != poolBatch.pool.segment_id) |
						static_cast<uint8_t>(location.generation != poolBatch.pool.generation) |
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
										  poolBatch.expected_replacements[index].page_id);
				publishedLocations.push_back(poolBatch.replacement_locations[index]);
			}
			for (size_t index = 0; index < poolBatch.appended_locations.size(); ++index) {
				validatePublishedLocation(poolBatch.appended_locations[index], poolBatch.appended_page_ids[index]);
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	PageId RelocatableMetadataStore::allocateLogicalPageId(LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const PageId pageId = batch.allocateLogicalPageId();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return pageId;
	}

	std::vector<PageId> RelocatableMetadataStore::allocateLogicalPageIds(size_t count, LogSequenceNumber checkpointLsn,
																		 const MetadataPublishOptions &options) {
		if (count == 0U || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable logical Page ID reservation requires a positive count and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<PageId> pageIds;
		pageIds.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			pageIds.push_back(batch.allocateLogicalPageId());
		}
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return pageIds;
	}

	std::vector<SegmentId> RelocatableMetadataStore::allocateLogicalSegmentIds(size_t count,
																			   LogSequenceNumber checkpointLsn,
																			   const MetadataPublishOptions &options) {
		if (count == 0U || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable logical Segment ID reservation requires a positive count and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<SegmentId> segmentIds;
		segmentIds.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			segmentIds.push_back(batch.allocateLogicalSegmentId());
		}
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return segmentIds;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalNodeId(LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t nodeId = batch.allocateLogicalNodeId();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return nodeId;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalEdgeId(LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t edgeId = batch.allocateLogicalEdgeId();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return edgeId;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalVectorId(LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t vectorId = batch.allocateLogicalVectorId();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return vectorId;
	}

	uint32_t RelocatableMetadataStore::allocatePropertyKeyToken(LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint32_t token = batch.allocatePropertyKeyToken();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return token;
	}

	void RelocatableMetadataStore::advancePropertyKeyTokenFloor(uint32_t token, LogSequenceNumber checkpointLsn,
																const MetadataPublishOptions &options) {
		if (token == 0U || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Property-key Token floor requires a nonzero Token and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.next_property_key_token == 0U || current.next_property_key_token > token) {
			return;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.observePropertyKeyToken(token);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
	}

	uint64_t RelocatableMetadataStore::allocateSchemaTokenId(LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t tokenId = batch.allocateSchemaTokenId();
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return tokenId;
	}

	void RelocatableMetadataStore::advanceSchemaTokenIdFloor(uint64_t tokenId, LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		if (tokenId == 0U || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable schema Token floor requires a nonzero Token ID and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.next_schema_token_id == 0U || current.next_schema_token_id > tokenId) {
			return;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.observeSchemaTokenId(tokenId);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
	}

	RetiredPhysicalSlotReclamation
	RelocatableMetadataStore::reclaimRetiredPhysicalSlots(LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options) {
		if (!checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Retire Queue reclamation requires a checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const std::vector<RetireQueueEntry> reclaimable = retire_queue_reclaimer_.collectReclaimable(current, writer);
		if (reclaimable.empty()) {
			return {current, 0U};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		retire_queue_reclaimer_.prepareReclamation(batch, reclaimable);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return {batch.candidate(), static_cast<uint64_t>(reclaimable.size())};
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
				requests, appendRequests, newPoolRequests, propertyReplacements, propertyPageBatchRequests, noLargeValues,
				derivedState, checkpointLsn,
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

	Superblock RelocatableMetadataStore::mutateNativeGraphPagesWithLargeValues(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			std::span<const NativeGraphLargeValueMutation> largeValues,
			const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
			const NativeGraphPageTransactionWithLargeValuesPreparer &preparer,
			const NativeGraphDerivedStatePreparer &derivedStatePreparer,
			const NativeGraphVectorBootstrapPreparer &vectorBootstrapPreparer,
			const NativeGraphVectorDeltaPreparer &vectorDeltaPreparer,
			std::span<const PageRetirement> retiredLargeValuePages, std::span<const LargeValueId> erasedLargeValueIds,
			const ExistingPoolPagesRetirementPreparer &largeValueRetirementPreparer,
			const MetadataPublishOptions &options, NativeGraphLogicalIdReservation logicalIdReservation,
			std::optional<Generation> expectedMetadataGeneration) {
		const bool hasLargeValueRetirements = !retiredLargeValuePages.empty() || !erasedLargeValueIds.empty();
		if (!preparer ||
			(requests.empty() && appendRequests.empty() && newPoolRequests.empty() && propertyReplacements.empty() &&
			 propertyPageBatchRequests.empty() && largeValues.empty() && !hasLargeValueRetirements &&
			 derivedState.empty() && logicalIdReservation.empty()) ||
			!checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph page transaction requires work, a checkpoint LSN, and a preparer");
		}
		if (hasLargeValueRetirements &&
			(retiredLargeValuePages.empty() || erasedLargeValueIds.empty() || !largeValueRetirementPreparer)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph LargeValue retirement requires pages, manifest IDs, and a typed preparer");
		}
		uint64_t previousErasedLargeValueId = 0U;
		for (const LargeValueId valueId: erasedLargeValueIds) {
			if (!valueId.isValid() || valueId.value() <= previousErasedLargeValueId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph LargeValue retirement has unordered Manifest identities");
			}
			previousErasedLargeValueId = valueId.value();
		}
		uint64_t previousRetiredLargeValuePageId = 0U;
		for (const PageRetirement &retirement: retiredLargeValuePages) {
			const PageLocation &location = retirement.location;
			const bool validOwner = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA ||
									retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			const uint8_t invalid =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousRetiredLargeValuePageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) | static_cast<uint8_t>(!validOwner) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(
							!std::ranges::binary_search(erasedLargeValueIds, retirement.expected_owner_id, {},
														[](LargeValueId valueId) { return valueId.value(); }));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph LargeValue retirement has an invalid or unrelated Page frame");
			}
			previousRetiredLargeValuePageId = location.page_id.value();
		}
		if (!derivedState.empty() && !derivedStatePreparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph derived-state publication requires a typed page preparer");
		}
		if ((!derivedState.vector_bootstraps.empty() && !vectorBootstrapPreparer) ||
			(derivedState.vector_bootstraps.empty() && vectorBootstrapPreparer)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph Vector bootstrap publication requires an exact typed preparer");
		}
		if ((!derivedState.vector_deltas.empty() && !vectorDeltaPreparer) ||
			(derivedState.vector_deltas.empty() && vectorDeltaPreparer)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph Vector delta publication requires an exact typed preparer");
		}
		uint64_t previousReusedLargeValueId = 0U;
		for (const LargeValueRef &reference: derivedState.reused_large_values) {
			if (!reference.value_id.isValid() || reference.value_id.value() <= previousReusedLargeValueId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph reused LargeValue references must have unique ordered identities");
			}
			static_cast<void>(LargeValueRefCodec::encode(reference));
			previousReusedLargeValueId = reference.value_id.value();
		}
		uint64_t previousIndexId = 0U;
		std::set<uint64_t> derivedSourcePageIds;
		std::set<uint64_t> derivedRetirementPageIds;
		for (const NativeGraphIndexTreeMutation &mutation: derivedState.index_tree_mutations) {
			const std::optional<IndexRootLocation> &expectedRoot = mutation.expected_root;
			const bool retiresRoot = mutation.retire_root;
			const uint8_t invalidRoot =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousIndexId) |
					static_cast<uint8_t>(expectedRoot.has_value() && (expectedRoot->index_id != mutation.index_id ||
																	  !expectedRoot->root_page_id.isValid() ||
																	  !expectedRoot->root_generation.isValid())) |
					static_cast<uint8_t>(!retiresRoot && mutation.pool_requests.empty() &&
										 mutation.new_pool_requests.empty()) |
					static_cast<uint8_t>(mutation.replacement_root.has_value() &&
										 mutation.stable_replacement_root.has_value());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph derived-state mutation has an invalid or unordered Index root");
			}
			// A fresh logical root may only use fresh physical pools. This keeps
			// first publication independent of an unrelated current pool and makes
			// a failed creation completely unreachable.
			if (!retiresRoot && !expectedRoot.has_value() &&
				(!mutation.pool_requests.empty() || !mutation.replacement_root.has_value() ||
				 !mutation.replacement_root->newly_reserved || mutation.stable_replacement_root.has_value())) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph derived-state root creation must publish one newly reserved Index page");
			}
			previousIndexId = mutation.index_id;

			std::set<uint64_t> mutationPoolIds;
			for (const IndexTreeMutationPoolRequest &pool: mutation.pool_requests) {
				const uint8_t invalidPool =
						static_cast<uint8_t>(!pool.segment_id.isValid()) |
						static_cast<uint8_t>(pool.expected_replacements.empty() && pool.appended_page_count == 0U &&
											 pool.expected_retirements.empty()) |
						static_cast<uint8_t>(pool.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(!mutationPoolIds.insert(pool.segment_id.value()).second);
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph derived-state mutation has an invalid or duplicate Index pool");
				}
				for (const PageLocation &location: pool.expected_replacements) {
					const uint8_t invalidLocation =
							static_cast<uint8_t>(!location.page_id.isValid()) |
							static_cast<uint8_t>(!derivedSourcePageIds.insert(location.page_id.value()).second) |
							static_cast<uint8_t>(derivedRetirementPageIds.contains(location.page_id.value()));
					if (invalidLocation != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph derived-state mutation has an invalid or duplicate Index "
											  "replacement page");
					}
				}
				for (const IndexTreePageRetirement &retirement: pool.expected_retirements) {
					const uint8_t invalidRetirement =
							static_cast<uint8_t>(!retirement.location.page_id.isValid()) |
							static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
							static_cast<uint8_t>(derivedSourcePageIds.contains(retirement.location.page_id.value())) |
							static_cast<uint8_t>(
									!derivedRetirementPageIds.insert(retirement.location.page_id.value()).second);
					if (invalidRetirement != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph derived-state mutation has an invalid or duplicate Index "
											  "retirement page");
					}
				}
			}
			for (const NewIndexTreePoolRequest &pool: mutation.new_pool_requests) {
				const uint8_t invalidPool =
						static_cast<uint8_t>(!pool.first_slot.isValid()) |
						static_cast<uint8_t>(!pool.last_slot.isValid()) |
						static_cast<uint8_t>(pool.first_slot > pool.last_slot) |
						static_cast<uint8_t>(pool.owner_id == 0U) |
						static_cast<uint8_t>(pool.appended_page_count == 0U) |
						static_cast<uint8_t>(pool.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph derived-state mutation has an invalid new Index pool request");
				}
			}
			if (mutation.replacement_root.has_value()) {
				const NativeGraphIndexTreeAppendedPageReference &reference = *mutation.replacement_root;
				const size_t poolCount =
						reference.newly_reserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				const size_t appendedCount = [&]() {
					if (reference.pool_ordinal >= poolCount) {
						return size_t{0U};
					}
					return reference.newly_reserved
								   ? mutation.new_pool_requests[reference.pool_ordinal].appended_page_count
								   : mutation.pool_requests[reference.pool_ordinal].appended_page_count;
				}();
				if (reference.pool_ordinal >= poolCount || reference.appended_page_ordinal >= appendedCount) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"native graph derived-state mutation has an invalid appended Index root reference");
				}
			}
			if (mutation.stable_replacement_root.has_value() &&
				(!mutation.stable_replacement_root->isValid() ||
				 derivedRetirementPageIds.contains(mutation.stable_replacement_root->value()))) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph derived-state mutation has an invalid stable Index root reference");
			}
			if (retiresRoot) {
				const bool invalidRetirement = !expectedRoot.has_value() || !mutation.new_pool_requests.empty() ||
											   mutation.replacement_root.has_value() ||
											   mutation.stable_replacement_root.has_value() ||
											   mutation.pool_requests.empty();
				bool rootRetired = false;
				for (const IndexTreeMutationPoolRequest &pool: mutation.pool_requests) {
					if (!pool.expected_replacements.empty() || pool.appended_page_count != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph root retirement cannot replace or append Index pages");
					}
					for (const IndexTreePageRetirement &retirement: pool.expected_retirements) {
						rootRetired = rootRetired || (expectedRoot.has_value() &&
													  retirement.location.page_id == expectedRoot->root_page_id &&
													  retirement.location.generation == expectedRoot->root_generation);
					}
				}
				if (invalidRetirement || !rootRetired) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph root retirement does not contain its pinned root page");
				}
			}
		}
		uint64_t previousVectorIndexId = 0U;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			const bool knownMetric = mutation.metric == VectorDistanceMetric::VDM_L2 ||
									 mutation.metric == VectorDistanceMetric::VDM_COSINE ||
									 mutation.metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
			const uint8_t malformed =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousVectorIndexId) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == 0U) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == mutation.index_id) |
					static_cast<uint8_t>(mutation.first_vector_id == 0U) |
					static_cast<uint8_t>(mutation.vector_count == 0U) |
					static_cast<uint8_t>(mutation.dimensions == 0U) | static_cast<uint8_t>(!knownMetric) |
					static_cast<uint8_t>(mutation.raw_page_count == 0U) |
					static_cast<uint8_t>(!mutation.first_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_slot > mutation.last_slot) |
					static_cast<uint8_t>(mutation.dimensions > (std::numeric_limits<uint32_t>::max() -
																static_cast<uint32_t>(sizeof(uint64_t))) /
																	   sizeof(uint16_t));
			if (malformed != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector bootstrap has an invalid identity, metric, resource bound, or stride");
			}
			const uint32_t recordBytes =
					static_cast<uint32_t>(sizeof(uint64_t)) + mutation.dimensions * sizeof(uint16_t);
			const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
			if (capacity == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector bootstrap Raw record does not fit a typed Vector page");
			}
			const uint64_t expectedPages = (mutation.vector_count - 1U) / static_cast<uint64_t>(capacity) + 1U;
			if (expectedPages != mutation.raw_page_count) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector bootstrap has a noncanonical Raw-page cardinality");
			}
			const auto hasFreshRoot = [&derivedState](uint64_t indexId) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->expected_root.has_value() &&
					   !found->retire_root && found->replacement_root.has_value() &&
					   found->replacement_root->newly_reserved;
			};
			if (!hasFreshRoot(mutation.index_id) || !hasFreshRoot(mutation.vector_node_mapping_index_id)) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector bootstrap requires two first-use immutable mapping roots");
			}
			previousVectorIndexId = mutation.index_id;
		}
		std::set<uint64_t> vectorPublicationIndexIds;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			vectorPublicationIndexIds.insert(mutation.index_id);
		}
		previousVectorIndexId = 0U;
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			const bool knownMetric = mutation.metric == VectorDistanceMetric::VDM_L2 ||
									 mutation.metric == VectorDistanceMetric::VDM_COSINE ||
									 mutation.metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
			const bool hasInserts = mutation.inserted_vector_count != 0U;
			const bool hasExpectedDelta = mutation.expected_delta_mapping_index_id != 0U;
			const bool publishesDelta = mutation.delta_mapping_index_id != 0U;
			const uint8_t malformed =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousVectorIndexId) |
					static_cast<uint8_t>(!vectorPublicationIndexIds.insert(mutation.index_id).second) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == 0U) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == mutation.index_id) |
					static_cast<uint8_t>(hasExpectedDelta != mutation.expected_delta_mapping_root.has_value()) |
					static_cast<uint8_t>(hasExpectedDelta && !VectorDeltaMapping::isMappingIndexId(
																	 mutation.expected_delta_mapping_index_id)) |
					static_cast<uint8_t>(mutation.expected_delta_mapping_root.has_value() &&
										 mutation.expected_delta_mapping_root->index_id !=
												 mutation.expected_delta_mapping_index_id) |
					static_cast<uint8_t>(mutation.expected_delta_mapping_root.has_value() &&
										 (!mutation.expected_delta_mapping_root->root_page_id.isValid() ||
										  !mutation.expected_delta_mapping_root->root_generation.isValid())) |
					static_cast<uint8_t>(publishesDelta != (mutation.delta_entry_count != 0U)) |
					static_cast<uint8_t>(publishesDelta &&
										 !VectorDeltaMapping::isMappingIndexId(mutation.delta_mapping_index_id)) |
					static_cast<uint8_t>(mutation.expected_node_mapping_root.index_id != mutation.index_id) |
					static_cast<uint8_t>(mutation.expected_vector_node_mapping_root.index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(!mutation.expected_node_mapping_root.root_page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_node_mapping_root.root_generation.isValid()) |
					static_cast<uint8_t>(!mutation.expected_vector_node_mapping_root.root_page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_vector_node_mapping_root.root_generation.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_pool.segment_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_location.page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_location.generation.isValid()) |
					static_cast<uint8_t>(mutation.expected_manifest.index_id != mutation.index_id) |
					static_cast<uint8_t>(mutation.expected_manifest.manifest_page_id !=
										 mutation.expected_manifest_location.page_id) |
					static_cast<uint8_t>(mutation.expected_manifest.generation !=
										 mutation.expected_manifest_location.generation) |
					static_cast<uint8_t>(!mutation.expected_manifest_owner_lsn.isValid()) |
					static_cast<uint8_t>(mutation.dimensions == 0U) | static_cast<uint8_t>(!knownMetric) |
					static_cast<uint8_t>(hasInserts && mutation.first_vector_id == 0U) |
					static_cast<uint8_t>(!hasInserts && mutation.first_vector_id != 0U) |
					static_cast<uint8_t>(hasInserts &&
										 mutation.inserted_vector_count - 1U >
												 std::numeric_limits<uint64_t>::max() - mutation.first_vector_id) |
					static_cast<uint8_t>(!mutation.first_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_slot > mutation.last_slot);
			if (malformed != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector delta has an invalid identity, fence, or resource bound");
			}
			const auto expectedMappingMutation = [&derivedState](uint64_t indexId, const IndexRootLocation &root) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->retire_root &&
					   found->expected_root.has_value() && *found->expected_root == root;
			};
			const auto freshMappingMutation = [&derivedState](uint64_t indexId) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->retire_root &&
					   !found->expected_root.has_value() && found->replacement_root.has_value() &&
					   found->replacement_root->newly_reserved;
			};
			const auto retirementMutation = [&derivedState](uint64_t indexId, const IndexRootLocation &root) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && found->retire_root &&
					   found->expected_root.has_value() && *found->expected_root == root;
			};
			if (!expectedMappingMutation(mutation.index_id, mutation.expected_node_mapping_root) ||
				!expectedMappingMutation(mutation.vector_node_mapping_index_id,
										 mutation.expected_vector_node_mapping_root) ||
				(hasExpectedDelta && publishesDelta &&
				 (!expectedMappingMutation(mutation.delta_mapping_index_id, *mutation.expected_delta_mapping_root) ||
				  mutation.delta_mapping_index_id != mutation.expected_delta_mapping_index_id)) ||
				(!hasExpectedDelta && publishesDelta && !freshMappingMutation(mutation.delta_mapping_index_id)) ||
				(hasExpectedDelta && !publishesDelta &&
				 !retirementMutation(mutation.expected_delta_mapping_index_id,
									 *mutation.expected_delta_mapping_root))) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector delta requires matching semantic and overlay mapping-tree transitions");
			}
			uint64_t previousErasedVectorId = 0U;
			for (const uint64_t vectorId: mutation.erased_vector_ids) {
				if (vectorId == 0U || vectorId <= previousErasedVectorId) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph Vector delta has unordered erased Vector IDs");
				}
				previousErasedVectorId = vectorId;
			}
			uint64_t insertedRecords = 0U;
			uint32_t expectedPageOrdinal = 0U;
			for (const NativeGraphVectorDeltaRawPageMutation &raw: mutation.raw_pages) {
				const bool cow = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
				const bool append = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL;
				const bool fresh = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL;
				const uint8_t invalidRaw = static_cast<uint8_t>(!cow && !append && !fresh) |
										   static_cast<uint8_t>(raw.page_ordinal != expectedPageOrdinal++) |
										   static_cast<uint8_t>(raw.inserted_record_count == 0U) |
										   static_cast<uint8_t>(cow && (!raw.expected_pool.segment_id.isValid() ||
																		!raw.expected_location.page_id.isValid() ||
																		!raw.expected_owner_lsn.isValid())) |
										   static_cast<uint8_t>(append && !raw.expected_pool.segment_id.isValid()) |
										   static_cast<uint8_t>(fresh && (raw.expected_pool.segment_id.isValid() ||
																		  raw.new_pool_ordinal == 0U));
				if (invalidRaw != 0U ||
					raw.inserted_record_count > std::numeric_limits<uint64_t>::max() - insertedRecords) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph Vector delta has an invalid Raw-page transition");
				}
				insertedRecords += raw.inserted_record_count;
			}
			if (insertedRecords != mutation.inserted_vector_count) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector delta Raw-page transitions do not cover its inserted Vector IDs");
			}
			previousVectorIndexId = mutation.index_id;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (expectedMetadataGeneration.has_value() && current.generation != *expectedMetadataGeneration) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph metadata publication no longer matches its pinned generation");
		}
		for (const LargeValueRef &reference: derivedState.reused_large_values) {
			// A prior snapshot authenticated the bytes. This writer-side fence only
			// proves that the immutable Manifest remains current before its small
			// Property reference can be made reachable.
			if (!locations_.findLargeValueManifestOptional(current, reference.value_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph reused LargeValue Manifest was retired before publication");
			}
		}
		uint64_t expectedVectorId = current.next_logical_vector_id;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			const uint8_t stale =
					static_cast<uint8_t>(expectedVectorId == 0U) |
					static_cast<uint8_t>(mutation.first_vector_id != expectedVectorId) |
					static_cast<uint8_t>(mutation.vector_count - 1U >
										 std::numeric_limits<uint64_t>::max() - mutation.first_vector_id) |
					static_cast<uint8_t>(locations_.findVectorManifestOptional(current, mutation.index_id).has_value());
			if (stale != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector bootstrap has a stale Manifest or noncontiguous Vector-ID reservation");
			}
			for (uint64_t offset = 0U; offset < mutation.vector_count; ++offset) {
				if (locations_.findVectorLocationOptional(current, mutation.first_vector_id + offset).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap reuses a published logical Vector ID");
				}
			}
			const uint64_t lastVectorId = mutation.first_vector_id + mutation.vector_count - 1U;
			expectedVectorId = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
		}
		std::vector<const NativeGraphVectorDeltaMutation *> vectorDeltasByVectorRange;
		vectorDeltasByVectorRange.reserve(derivedState.vector_deltas.size());
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			vectorDeltasByVectorRange.push_back(&mutation);
		}
		std::ranges::sort(vectorDeltasByVectorRange, [](const NativeGraphVectorDeltaMutation *left,
														const NativeGraphVectorDeltaMutation *right) {
			const uint64_t leftFirst =
					left->inserted_vector_count == 0U ? std::numeric_limits<uint64_t>::max() : left->first_vector_id;
			const uint64_t rightFirst =
					right->inserted_vector_count == 0U ? std::numeric_limits<uint64_t>::max() : right->first_vector_id;
			return std::tuple<uint64_t, uint64_t>{leftFirst, left->index_id} <
				   std::tuple<uint64_t, uint64_t>{rightFirst, right->index_id};
		});
		for (const NativeGraphVectorDeltaMutation *mutation: vectorDeltasByVectorRange) {
			const std::optional<IndexRootLocation> nodeRoot =
					locations_.findIndexRootOptional(current, mutation->index_id);
			const std::optional<IndexRootLocation> reverseRoot =
					locations_.findIndexRootOptional(current, mutation->vector_node_mapping_index_id);
			const std::optional<IndexRootLocation> deltaRoot =
					mutation->expected_delta_mapping_root.has_value()
							? locations_.findIndexRootOptional(current, mutation->expected_delta_mapping_index_id)
							: std::nullopt;
			const std::optional<VectorManifestLocation> manifestDirectory =
					locations_.findVectorManifestOptional(current, mutation->index_id);
			const PageLocation manifestLocation =
					locations_.findPage(current, mutation->expected_manifest_location.page_id);
			const CatalogEntry manifestPool = catalog_.find(current, manifestLocation.segment_id);
			const uint8_t stale =
					static_cast<uint8_t>(!nodeRoot.has_value() || *nodeRoot != mutation->expected_node_mapping_root) |
					static_cast<uint8_t>(!reverseRoot.has_value() ||
										 *reverseRoot != mutation->expected_vector_node_mapping_root) |
					static_cast<uint8_t>(mutation->expected_delta_mapping_root.has_value() != deltaRoot.has_value()) |
					static_cast<uint8_t>(deltaRoot.has_value() &&
										 *deltaRoot != *mutation->expected_delta_mapping_root) |
					static_cast<uint8_t>(!manifestDirectory.has_value() ||
										 *manifestDirectory != mutation->expected_manifest) |
					static_cast<uint8_t>(manifestLocation != mutation->expected_manifest_location) |
					static_cast<uint8_t>(!hasSameCatalogEntry(manifestPool, mutation->expected_manifest_pool)) |
					static_cast<uint8_t>(manifestPool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
					static_cast<uint8_t>(manifestPool.owner_id != mutation->index_id) |
					static_cast<uint8_t>(manifestPool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, manifestPool.physical_slot));
			if (stale != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta has a stale Manifest or mapping-root fence");
			}
			for (const uint64_t vectorId: mutation->erased_vector_ids) {
				if (!locations_.findVectorLocationOptional(current, vectorId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta erases an unreachable logical Vector ID");
				}
			}
			if (mutation->inserted_vector_count == 0U) {
				continue;
			}
			const uint8_t invalidRange =
					static_cast<uint8_t>(expectedVectorId == 0U) |
					static_cast<uint8_t>(mutation->first_vector_id != expectedVectorId) |
					static_cast<uint8_t>(mutation->inserted_vector_count - 1U >
										 std::numeric_limits<uint64_t>::max() - mutation->first_vector_id);
			if (invalidRange != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta has a noncontiguous logical Vector-ID reservation");
			}
			for (uint64_t offset = 0U; offset < mutation->inserted_vector_count; ++offset) {
				if (locations_.findVectorLocationOptional(current, mutation->first_vector_id + offset).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta reuses a published logical Vector ID");
				}
			}
			const uint64_t lastVectorId = mutation->first_vector_id + mutation->inserted_vector_count - 1U;
			expectedVectorId = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
		}
		uint64_t expectedLargeValueId = current.next_large_value_id;
		for (const NativeGraphLargeValueMutation &mutation: largeValues) {
			const bool validCodec = mutation.reference.codec == LargeValueCodec::LVC_RAW ||
									mutation.reference.codec == LargeValueCodec::LVC_LZ4 ||
									mutation.reference.codec == LargeValueCodec::LVC_ZSTD ||
									mutation.reference.codec == LargeValueCodec::LVC_MIXED;
			const uint8_t invalid =
					static_cast<uint8_t>(expectedLargeValueId == 0U) |
					static_cast<uint8_t>(mutation.reference.value_id.value() != expectedLargeValueId) |
					static_cast<uint8_t>(mutation.reference.generation != Generation(1U)) |
					static_cast<uint8_t>(mutation.reference.logical_bytes != mutation.logical_bytes.size()) |
					static_cast<uint8_t>(mutation.planned_manifest_page_count == 0U) |
					static_cast<uint8_t>(!validCodec) |
					static_cast<uint8_t>(mutation.logical_bytes.empty() != (mutation.planned_data_page_count == 0U)) |
					static_cast<uint8_t>(!mutation.first_data_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_data_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_data_slot > mutation.last_data_slot) |
					static_cast<uint8_t>(!mutation.first_manifest_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_manifest_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_manifest_slot > mutation.last_manifest_slot) |
					static_cast<uint8_t>(utils::Sha256::digest(mutation.logical_bytes) !=
										 mutation.reference.content_hash) |
					static_cast<uint8_t>(locations_.findLargeValueManifestOptional(current, mutation.reference.value_id)
												 .has_value());
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue mutation is stale, malformed, or already reachable");
			}
			static_cast<void>(LargeValueRefCodec::encode(mutation.reference));
			if (expectedLargeValueId == std::numeric_limits<uint64_t>::max()) {
				if (&mutation != &largeValues.back()) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "native graph LargeValue mutation exhausts the logical value ID space");
				}
			} else {
				++expectedLargeValueId;
			}
		}
		std::set<uint64_t> pageIds;
		std::vector<ExistingPoolPageRetirementBatch> largeValueRetirementBatches;
		if (hasLargeValueRetirements) {
			for (const LargeValueId valueId: erasedLargeValueIds) {
				const LargeValueManifestLocation manifest = locations_.findLargeValueManifest(current, valueId);
				const auto retirement = std::ranges::find_if(
						retiredLargeValuePages, [&manifest, valueId](const PageRetirement &candidate) {
							return candidate.location.page_id == manifest.manifest_page_id &&
								   candidate.expected_owner_id == valueId.value() &&
								   candidate.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
						});
				if (retirement == retiredLargeValuePages.end()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue retirement omits one current Manifest frame");
				}
			}
			std::map<uint64_t, ExistingPoolPageRetirementBatch> batchesBySegment;
			for (const PageRetirement &retirement: retiredLargeValuePages) {
				const PageLocation visibleLocation = locations_.findPage(current, retirement.location.page_id);
				const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
				const SegmentKind expectedKind = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA
												 ? SegmentKind::SGK_LARGE_VALUE_DATA_POOL
												 : SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				const uint64_t expectedPoolOwner =
						retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA
								? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID
								: RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				const uint8_t invalid =
						static_cast<uint8_t>(visibleLocation != retirement.location) |
						static_cast<uint8_t>(pool.kind != expectedKind) |
						static_cast<uint8_t>(pool.owner_id != expectedPoolOwner) |
						static_cast<uint8_t>(pool.generation != retirement.location.generation) |
						static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot)) |
						static_cast<uint8_t>(!pageIds.insert(retirement.location.page_id.value()).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue retirement has a stale or unavailable Page frame");
				}
				auto [iterator, inserted] = batchesBySegment.try_emplace(pool.segment_id.value());
				if (inserted) {
					iterator->second.pool = pool;
				}
				iterator->second.retirements.push_back(retirement);
			}
			largeValueRetirementBatches.reserve(batchesBySegment.size());
			for (auto &[segmentId, batch]: batchesBySegment) {
				static_cast<void>(segmentId);
				largeValueRetirementBatches.push_back(std::move(batch));
			}
		}
		std::set<uint64_t> insertedNodeIds;
		std::set<uint64_t> erasedNodeIds;
		std::set<uint64_t> insertedEdgeIds;
		std::set<uint64_t> erasedEdgeIds;
		std::vector<EntityPageLocation> insertedNodes;
		std::vector<uint64_t> erasedNodes;
		std::vector<EntityPageLocation> insertedEdges;
		std::vector<uint64_t> erasedEdges;
		std::vector<uint64_t> retiredPageIds;
		retiredPageIds.reserve(retiredLargeValuePages.size());
		for (const PageRetirement &retirement: retiredLargeValuePages) {
			retiredPageIds.push_back(retirement.location.page_id.value());
		}
		std::vector<EntityPageReplacementMaterialization> materializations;
		std::vector<std::optional<EntityPageFreshPoolDestination>> replacementFreshDestinations;
		std::vector<EntityPageAppendBatchMaterialization> appendMaterializations;
		std::vector<EntityPageNewPoolBatchMaterialization> newPoolMaterializations;
		std::vector<NativeGraphPropertyPageReplacementMaterialization> propertyMaterializations;
		std::vector<NativeGraphPropertyPageBatchMaterialization> propertyPageBatchMaterializations;
		std::vector<LargeValueMaterializationResult> largeValueMaterializations;
		materializations.reserve(requests.size());
		replacementFreshDestinations.reserve(requests.size());
		appendMaterializations.reserve(appendRequests.size());
		newPoolMaterializations.reserve(newPoolRequests.size());
		propertyMaterializations.reserve(propertyReplacements.size());
		propertyPageBatchMaterializations.reserve(propertyPageBatchRequests.size());
		largeValueMaterializations.reserve(largeValues.size());

		// Revalidate the pinned B+Tree roots and every existing page/pool fence
		// while this writer owns the current metadata generation. New pools and
		// PageIds are assigned later from the same MetadataWriteBatch as Entity
		// and Property work, so none can become visible independently.
		std::vector<std::vector<IndexTreeMutationPoolBatch>> derivedIndexBatches;
		derivedIndexBatches.reserve(derivedState.index_tree_mutations.size());
		for (const NativeGraphIndexTreeMutation &mutation: derivedState.index_tree_mutations) {
			const std::optional<IndexRootLocation> visibleRoot =
					locations_.findIndexRootOptional(current, mutation.index_id);
			if (mutation.expected_root.has_value()) {
				if (!visibleRoot.has_value() || *visibleRoot != *mutation.expected_root) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation has a stale Index root mapping");
				}
				const PageLocation rootLocation = locations_.findPage(current, mutation.expected_root->root_page_id);
				const CatalogEntry rootPool = catalog_.find(current, rootLocation.segment_id);
				const uint8_t invalidRoot =
						static_cast<uint8_t>(rootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(rootPool.owner_id == 0U) |
						static_cast<uint8_t>(rootPool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(rootPool.generation != rootLocation.generation) |
						static_cast<uint8_t>(rootLocation.generation != mutation.expected_root->root_generation) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, rootPool.physical_slot));
				if (invalidRoot != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state mutation root does not resolve to one current Index pool");
				}
			} else if (visibleRoot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph derived-state root creation found an already published Index root");
			}

			std::vector<IndexTreeMutationPoolBatch> batches;
			batches.reserve(mutation.pool_requests.size() + mutation.new_pool_requests.size());
			for (const IndexTreeMutationPoolRequest &request: mutation.pool_requests) {
				const CatalogEntry pool = catalog_.find(current, request.segment_id);
				const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
											static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation requires current owned Index pools");
				}
				const auto validateSource = [this, &current, &pool, &pageIds](const PageLocation &location,
																			  const char *message) {
					const PageLocation visible = locations_.findPage(current, location.page_id);
					const uint8_t invalid = static_cast<uint8_t>(visible != location) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(location.generation != pool.generation) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(!pageIds.insert(location.page_id.value()).second);
					if (invalid != 0U) {
						throw FormatException(FormatError::FERR_INVALID_STATE, message);
					}
				};
				for (const PageLocation &location: request.expected_replacements) {
					validateSource(
							location,
							"native graph derived-state mutation has a stale or duplicate Index replacement page");
				}
				for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
					validateSource(
							retirement.location,
							"native graph derived-state mutation has a stale or duplicate Index retirement page");
				}
				batches.push_back({pool,
								   request.expected_replacements,
								   {},
								   request.expected_replacements,
								   {},
								   false,
								   request.expected_retirements});
			}
			derivedIndexBatches.push_back(std::move(batches));
		}

		const auto validateInserted = [this, &current](std::span<const EntityPageLocation> locations, PageId pageId,
													   bool nodes, std::set<uint64_t> &seen, const char *message) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing =
						nodes ? locations_.findNodePageLocationOptional(current, location.entity_id)
							  : locations_.findEdgePageLocationOptional(current, location.entity_id);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value()) |
										static_cast<uint8_t>(!seen.insert(location.entity_id).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				previous = location.entity_id;
			}
		};
		const auto validateErasures = [this, &current](std::span<const uint64_t> entityIds, PageId pageId, bool nodes,
													   std::set<uint64_t> &seen, const char *message) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> existing =
						nodes ? locations_.findNodePageLocationOptional(current, entityId)
							  : locations_.findEdgePageLocationOptional(current, entityId);
				const uint8_t invalid = static_cast<uint8_t>(entityId == 0U) |
										static_cast<uint8_t>(entityId <= previous) |
										static_cast<uint8_t>(!existing.has_value()) |
										static_cast<uint8_t>(existing.has_value() && existing->page_id != pageId) |
										static_cast<uint8_t>(!seen.insert(entityId).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				previous = entityId;
			}
		};

		std::map<uint64_t, EntityPageFreshPoolDestination> freshReplacementPools;
		for (const EntityPageReplacementRequest &request: requests) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const bool hasReplacementPool = request.replacement_pool.segment_id.isValid();
			const bool hasFreshReplacementPool = request.replacement_new_pool.has_value();
			const bool malformedAbsentReplacementPool =
					!hasReplacementPool && !hasFreshReplacementPool &&
					(request.replacement_pool.physical_slot.isValid() ||
					 request.replacement_pool.kind != SegmentKind::SGK_NONE ||
					 request.replacement_pool.owner_id != 0U || request.replacement_pool.generation.isValid() ||
					 request.replacement_pool.state != SegmentState::SGS_FREE ||
					 request.replacement_pool.committed_lsn.isValid());
			const CatalogEntry replacementPool =
					hasReplacementPool ? catalog_.find(current, request.replacement_pool.segment_id) : visible;
			const EntityPageFreshPoolDestination freshPool =
					hasFreshReplacementPool ? *request.replacement_new_pool : EntityPageFreshPoolDestination{};
			const uint8_t invalidFreshReplacementPool = static_cast<uint8_t>(
					hasFreshReplacementPool &&
					(!freshPool.segment_id.isValid() ||
					 freshPool.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID || freshPool.pool_owner_id == 0U ||
					 !freshPool.generation.isValid() || !freshPool.first_slot.isValid() ||
					 !freshPool.last_slot.isValid() || freshPool.first_slot > freshPool.last_slot ||
					 catalog_.findOptional(current, freshPool.segment_id).has_value()));
			if (invalidFreshReplacementPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch fresh replacement pool is malformed or published");
			}
			if (hasFreshReplacementPool) {
				auto [iterator, inserted] = freshReplacementPools.emplace(freshPool.segment_id.value(), freshPool);
				if (!inserted && !hasSameFreshEntityPoolIdentity(iterator->second, freshPool)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page batch reuses a fresh replacement Segment ID inconsistently");
				}
			}
			const PageLocation currentLocation = locations_.findPage(current, request.expected_location.page_id);
			const bool changesNodes = !request.inserted_nodes.empty() || !request.erased_node_ids.empty();
			const bool changesEdges = !request.inserted_edges.empty() || !request.erased_edge_ids.empty();
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidRetirement =
					request.retire_page &&
					(isAuxiliaryPage ? hasEntityMapDelta(request)
									 : (!request.inserted_nodes.empty() || !request.inserted_edges.empty() ||
										(!request.erased_node_ids.empty() == !request.erased_edge_ids.empty())));
			const uint8_t invalid =
					static_cast<uint8_t>(visible.segment_id != request.expected_pool.segment_id) |
					static_cast<uint8_t>(visible.physical_slot != request.expected_pool.physical_slot) |
					static_cast<uint8_t>(visible.kind != request.expected_pool.kind) |
					static_cast<uint8_t>(visible.owner_id != request.expected_pool.owner_id) |
					static_cast<uint8_t>(visible.generation != request.expected_pool.generation) |
					static_cast<uint8_t>(visible.committed_lsn != request.expected_pool.committed_lsn) |
					static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
					static_cast<uint8_t>(malformedAbsentReplacementPool) |
					static_cast<uint8_t>(hasReplacementPool && hasFreshReplacementPool) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.physical_slot != replacementPool.physical_slot) |
					static_cast<uint8_t>(hasReplacementPool && request.replacement_pool.kind != replacementPool.kind) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.owner_id != replacementPool.owner_id) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.generation != replacementPool.generation) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.state != replacementPool.state) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.committed_lsn != replacementPool.committed_lsn) |
					static_cast<uint8_t>(replacementPool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(replacementPool.owner_id != visible.owner_id) |
					static_cast<uint8_t>(hasFreshReplacementPool && freshPool.pool_owner_id != visible.owner_id) |
					static_cast<uint8_t>(replacementPool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!replacementPool.generation.isValid()) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, replacementPool.physical_slot)) |
					static_cast<uint8_t>(currentLocation != request.expected_location) |
					static_cast<uint8_t>(request.expected_location.segment_id != visible.segment_id) |
					static_cast<uint8_t>(request.expected_location.generation != visible.generation) |
					static_cast<uint8_t>(request.expected_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(request.expected_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(request.owner_kind)) |
					static_cast<uint8_t>(isAuxiliaryPage && hasEntityMapDelta(request)) |
					static_cast<uint8_t>(changesNodes && changesEdges) | static_cast<uint8_t>(invalidRetirement) |
					static_cast<uint8_t>(request.retire_page && (hasReplacementPool || hasFreshReplacementPool)) |
					static_cast<uint8_t>(!pageIds.insert(request.expected_location.page_id.value()).second);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch replacement has a stale pool, duplicate page, or "
									  "invalid map delta");
			}
			validateInserted(
					request.inserted_nodes, request.expected_location.page_id, true, insertedNodeIds,
					"relocatable Entity page batch replacement has an invalid or published Node Entity Map entry");
			validateErasures(
					request.erased_node_ids, request.expected_location.page_id, true, erasedNodeIds,
					"relocatable Entity page batch replacement erases an absent or foreign Node Entity Map entry");
			validateInserted(
					request.inserted_edges, request.expected_location.page_id, false, insertedEdgeIds,
					"relocatable Entity page batch replacement has an invalid or published Edge Entity Map entry");
			validateErasures(
					request.erased_edge_ids, request.expected_location.page_id, false, erasedEdgeIds,
					"relocatable Entity page batch replacement erases an absent or foreign Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			erasedNodes.insert(erasedNodes.end(), request.erased_node_ids.begin(), request.erased_node_ids.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
			erasedEdges.insert(erasedEdges.end(), request.erased_edge_ids.begin(), request.erased_edge_ids.end());
			materializations.push_back({replacementPool, currentLocation,
										request.retire_page
												? PageLocation{}
												: PageLocation{currentLocation.page_id, replacementPool.segment_id, 0U,
															   replacementPool.generation, 0U},
										request.retire_page, hasFreshReplacementPool, request.owner_kind});
			replacementFreshDestinations.push_back(request.replacement_new_pool);
		}
		for (const EntityPageAppendBatchRequest &request: appendRequests) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidMapShape = isAuxiliaryPage
												 ? (!request.inserted_nodes.empty() || !request.inserted_edges.empty())
												 : (request.inserted_nodes.empty() == request.inserted_edges.empty());
			const bool invalid =
					!request.page_id.isValid() || invalidMapShape || !isEntityPoolPageOwnerKind(request.owner_kind) ||
					visible.segment_id != request.expected_pool.segment_id ||
					visible.physical_slot != request.expected_pool.physical_slot ||
					visible.kind != request.expected_pool.kind || visible.owner_id != request.expected_pool.owner_id ||
					visible.generation != request.expected_pool.generation ||
					visible.committed_lsn != request.expected_pool.committed_lsn ||
					visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL || visible.state != SegmentState::SGS_SEALED ||
					!allocator_.isAllocated(current, visible.physical_slot) ||
					locations_.findPageOptional(current, request.page_id).has_value() ||
					!pageIds.insert(request.page_id.value()).second;
			if (invalid) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch append has a stale pool, published Page ID, or "
									  "invalid map delta");
			}
			validateInserted(request.inserted_nodes, request.page_id, true, insertedNodeIds,
							 "relocatable Entity page batch append has an invalid or published Node Entity Map entry");
			validateInserted(request.inserted_edges, request.page_id, false, insertedEdgeIds,
							 "relocatable Entity page batch append has an invalid or published Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
			appendMaterializations.push_back(
					{visible, {request.page_id, visible.segment_id, 0U, visible.generation, 0U}, request.owner_kind});
		}
		// A fresh Entity pool may start with several typed pages (for example, a
		// Node page and a LabelSet page). They share exactly one Catalog identity
		// and physical slot, while every PageId remains independently validated.
		std::map<uint64_t, size_t> firstNewPoolRequestBySegment;
		std::map<uint64_t, size_t> newPoolInitialFrameCounts;
		for (size_t requestOrdinal = 0U; requestOrdinal < newPoolRequests.size(); ++requestOrdinal) {
			const EntityPageNewPoolBatchRequest &request = newPoolRequests[requestOrdinal];
			const CatalogSlotReservationRequest &reservation = request.reservation;
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidMapShape = isAuxiliaryPage
												 ? (!request.inserted_nodes.empty() || !request.inserted_edges.empty())
												 : (request.inserted_nodes.empty() == request.inserted_edges.empty());
			const auto [firstRequest, firstForSegment] =
					firstNewPoolRequestBySegment.try_emplace(reservation.segment_id.value(), requestOrdinal);
			const bool conflictingPoolIdentity =
					!firstForSegment && !hasSameNewEntityPoolIdentity(request, newPoolRequests[firstRequest->second]);
			const size_t initialFrameCount = ++newPoolInitialFrameCounts[reservation.segment_id.value()];
			const bool invalid = !reservation.segment_id.isValid() ||
								 reservation.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID ||
								 reservation.kind != SegmentKind::SGK_ENTITY_PAGE_POOL || reservation.owner_id == 0U ||
								 !reservation.segment_generation.isValid() ||
								 reservation.state != SegmentState::SGS_SEALED || !reservation.require_absent ||
								 !request.page_id.isValid() || !request.first_slot.isValid() ||
								 !request.last_slot.isValid() || request.first_slot > request.last_slot ||
								 invalidMapShape || !isEntityPoolPageOwnerKind(request.owner_kind) ||
								 catalog_.findOptional(current, reservation.segment_id).has_value() ||
								 conflictingPoolIdentity || initialFrameCount > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 locations_.findPageOptional(current, request.page_id).has_value() ||
								 !pageIds.insert(request.page_id.value()).second;
			if (invalid) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool batch has a published identity or invalid pool request");
			}
			validateInserted(
					request.inserted_nodes, request.page_id, true, insertedNodeIds,
					"relocatable Entity page new-pool batch has an invalid or published Node Entity Map entry");
			validateInserted(
					request.inserted_edges, request.page_id, false, insertedEdgeIds,
					"relocatable Entity page new-pool batch has an invalid or published Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
		}
		std::set<uint64_t> deletedEntityPropertyIds;
		const auto registerDeletedEntityProperties = [&deletedEntityPropertyIds](std::span<const uint64_t> entityIds,
																				 EntityPropertyOwnerKind ownerKind) {
			for (const uint64_t entityId: entityIds) {
				const EntityPropertyKey key{ownerKind, entityId};
				if (!key.isValid() || !deletedEntityPropertyIds.insert(key.directoryId()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph page transaction has duplicate deleted Entity Property owners");
				}
			}
		};
		registerDeletedEntityProperties(erasedNodes, EntityPropertyOwnerKind::EPOK_NODE);
		registerDeletedEntityProperties(erasedEdges, EntityPropertyOwnerKind::EPOK_EDGE);
		const auto ownerSurvivesTransaction = [this, &current, &insertedNodeIds, &erasedNodeIds, &insertedEdgeIds,
											   &erasedEdgeIds](const EntityPropertyKey &key) {
			switch (key.owner_kind) {
				case EntityPropertyOwnerKind::EPOK_NODE:
					return !erasedNodeIds.contains(key.entity_id) &&
						   (insertedNodeIds.contains(key.entity_id) ||
							locations_.findNodePageLocationOptional(current, key.entity_id).has_value());
				case EntityPropertyOwnerKind::EPOK_EDGE:
					return !erasedEdgeIds.contains(key.entity_id) &&
						   (insertedEdgeIds.contains(key.entity_id) ||
							locations_.findEdgePageLocationOptional(current, key.entity_id).has_value());
			}
			return false;
		};

		std::set<uint64_t> touchedPropertyKeys;
		std::set<std::pair<uint64_t, uint16_t>> publishedPropertySlots;
		std::vector<EntityPropertyLocation> upsertedEntityProperties;
		std::vector<EntityPropertyKey> explicitlyErasedEntityProperties;
		std::map<uint64_t, EntityPropertyLocation> relocatedPropertySources;
		for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacements) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const PageLocation currentLocation = locations_.findPage(current, request.expected_location.page_id);
			const bool invalidRetirement = request.retire_page &&
					(!request.upserted_properties.empty() ||
					 (request.erased_properties.empty() && request.relocated_properties.empty()));
			const uint8_t invalidSource =
					static_cast<uint8_t>(visible.segment_id != request.expected_pool.segment_id) |
					static_cast<uint8_t>(visible.physical_slot != request.expected_pool.physical_slot) |
					static_cast<uint8_t>(visible.kind != request.expected_pool.kind) |
					static_cast<uint8_t>(visible.owner_id != request.expected_pool.owner_id) |
					static_cast<uint8_t>(visible.generation != request.expected_pool.generation) |
					static_cast<uint8_t>(visible.committed_lsn != request.expected_pool.committed_lsn) |
					static_cast<uint8_t>(visible.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
					static_cast<uint8_t>(currentLocation != request.expected_location) |
					static_cast<uint8_t>(request.expected_location.segment_id != visible.segment_id) |
					static_cast<uint8_t>(request.expected_location.generation != visible.generation) |
					static_cast<uint8_t>(request.expected_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(request.expected_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(request.expected_page_owner_id == 0U) |
					static_cast<uint8_t>(!request.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(request.upserted_properties.empty() && request.erased_properties.empty() &&
									 request.relocated_properties.empty()) |
					static_cast<uint8_t>(invalidRetirement) |
					static_cast<uint8_t>(!pageIds.insert(request.expected_location.page_id.value()).second);
			if (invalidSource != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph page transaction has a stale or invalid Property page replacement source");
			}
			for (const EntityPropertyLocation &location: request.upserted_properties) {
				const std::optional<EntityPropertyLocation> existing =
						locations_.findEntityPropertyOptional(current, location.key);
				const uint8_t invalidLocation =
						static_cast<uint8_t>(!location.key.isValid()) |
						static_cast<uint8_t>(!ownerSurvivesTransaction(location.key)) |
						static_cast<uint8_t>(location.page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(existing.has_value() &&
											 existing->page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(deletedEntityPropertyIds.contains(location.key.directoryId())) |
						static_cast<uint8_t>(relocatedPropertySources.contains(location.key.directoryId())) |
						static_cast<uint8_t>(!touchedPropertyKeys.insert(location.key.directoryId()).second) |
						static_cast<uint8_t>(
								!publishedPropertySlots.emplace(location.page_id.value(), location.slot_id).second);
				if (invalidLocation != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property directory upsert");
				}
				upsertedEntityProperties.push_back(location);
			}
			for (const EntityPropertyKey &key: request.erased_properties) {
				const std::optional<EntityPropertyLocation> existing =
						locations_.findEntityPropertyOptional(current, key);
				const uint8_t invalidErase =
						static_cast<uint8_t>(!key.isValid()) | static_cast<uint8_t>(!existing.has_value()) |
						static_cast<uint8_t>(existing.has_value() &&
											 existing->page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(relocatedPropertySources.contains(key.directoryId())) |
						static_cast<uint8_t>(!touchedPropertyKeys.insert(key.directoryId()).second);
				if (invalidErase != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property directory deletion");
				}
				explicitlyErasedEntityProperties.push_back(key);
			}
			for (const EntityPropertyKey &key: request.relocated_properties) {
				const std::optional<EntityPropertyLocation> existing =
						locations_.findEntityPropertyOptional(current, key);
				const uint64_t directoryId = key.isValid() ? key.directoryId() : 0U;
				const bool invalidRelocation =
						!key.isValid() || !ownerSurvivesTransaction(key) || !existing.has_value() ||
						(existing.has_value() && existing->page_id != request.expected_location.page_id) ||
						deletedEntityPropertyIds.contains(directoryId) || touchedPropertyKeys.contains(directoryId) ||
						relocatedPropertySources.contains(directoryId);
				if (invalidRelocation) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property relocation source");
				}
				relocatedPropertySources.emplace(directoryId, *existing);
			}
			propertyMaterializations.push_back(
					{visible, currentLocation,
					 request.retire_page
							 ? PageLocation{}
							 : PageLocation{currentLocation.page_id, visible.segment_id, 0U, visible.generation, 0U},
					 request.expected_page_owner_id, request.expected_owner_lsn, request.retire_page});
		}
		std::set<uint64_t> matchedPropertyRelocations;
		std::map<uint64_t, size_t> propertyReplacementFramesByPool;
		for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacements) {
			if (!request.retire_page) {
				++propertyReplacementFramesByPool[request.expected_pool.segment_id.value()];
			}
		}
		std::set<uint64_t> appendedPropertyPools;
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			const bool appendsExisting =
					request.kind == NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL;
			const bool createsPool = request.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
			const bool emptyExpectedPool =
					!request.expected_pool.segment_id.isValid() && !request.expected_pool.physical_slot.isValid() &&
					request.expected_pool.kind == SegmentKind::SGK_NONE && request.expected_pool.owner_id == 0U &&
					!request.expected_pool.generation.isValid() &&
					request.expected_pool.state == SegmentState::SGS_FREE &&
					!request.expected_pool.committed_lsn.isValid();
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!appendsExisting && !createsPool) |
					static_cast<uint8_t>(request.page_owner_id == 0U) |
					static_cast<uint8_t>(request.page_property_keys.empty()) |
					static_cast<uint8_t>(request.page_property_keys.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(appendsExisting &&
									 (request.first_slot.isValid() || request.last_slot.isValid())) |
					static_cast<uint8_t>(createsPool && !emptyExpectedPool) |
					static_cast<uint8_t>(createsPool && !request.first_slot.isValid()) |
					static_cast<uint8_t>(createsPool && !request.last_slot.isValid()) |
					static_cast<uint8_t>(createsPool && request.first_slot > request.last_slot);
			if (invalidRequest != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Property page batch has an invalid kind, owner, pool, slot range, or page set");
			}
			if (appendsExisting) {
				const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
				const size_t replacementFrames =
						propertyReplacementFramesByPool[visible.segment_id.value()];
				const size_t availableFrames = SharedPagePoolAllocator::availableFrameCountUnderWrite(
						*io_, current.data_region_offset, visible);
				const bool invalidCapacity = request.page_property_keys.size() > availableFrames ||
						replacementFrames > availableFrames - request.page_property_keys.size();
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(visible, request.expected_pool)) |
						static_cast<uint8_t>(visible.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
						static_cast<uint8_t>(visible.owner_id != request.page_owner_id) |
						static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
						static_cast<uint8_t>(invalidCapacity) |
						static_cast<uint8_t>(!appendedPropertyPools.insert(visible.segment_id.value()).second);
				if (invalidPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property append batch has a stale, duplicate, or capacity-exhausted pool");
				}
			}
			uint64_t previousKey = 0U;
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				if (pageKeys.empty()) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Property page batch contains an empty page");
				}
				for (const EntityPropertyKey &key: pageKeys) {
					const std::optional<EntityPropertyLocation> existing =
							locations_.findEntityPropertyOptional(current, key);
					const uint64_t directoryId = key.isValid() ? key.directoryId() : 0U;
					const auto relocation = relocatedPropertySources.find(directoryId);
					const bool relocatesExisting = existing.has_value();
					const bool invalidKey =
							!key.isValid() || !ownerSurvivesTransaction(key) || directoryId <= previousKey ||
							deletedEntityPropertyIds.contains(directoryId) ||
							(relocatesExisting &&
							 (relocation == relocatedPropertySources.end() || relocation->second != *existing)) ||
							(!relocatesExisting && relocation != relocatedPropertySources.end());
					if (invalidKey || !touchedPropertyKeys.insert(directoryId).second ||
						(relocatesExisting && !matchedPropertyRelocations.insert(directoryId).second)) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph Property page batch has an unmatched, unordered, or conflicting owner key");
					}
					previousKey = directoryId;
				}
			}
		}
		if (matchedPropertyRelocations.size() != relocatedPropertySources.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "native graph Property relocation is missing its destination root publication");
		}

		std::ranges::sort(insertedNodes, {}, &EntityPageLocation::entity_id);
		std::ranges::sort(erasedNodes);
		std::ranges::sort(insertedEdges, {}, &EntityPageLocation::entity_id);
		std::ranges::sort(erasedEdges);
		const auto validateLeaseRanges = [](std::span<const NativeGraphLogicalIdLeaseRange> ranges,
														 uint64_t currentHighWater, const char *entityName) {
			uint64_t previousLast = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  std::string("native graph ") + entityName +
												  " lease range is invalid or overflows the logical ID space");
				}
				const uint64_t last = range.first_id + range.count - 1U;
				if ((previousLast != 0U && range.first_id <= previousLast) ||
					(currentHighWater != 0U && last >= currentHighWater)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  std::string("native graph ") + entityName +
												  " lease range is not durably reserved by the current high-water mark");
				}
				previousLast = last;
			}
		};
		const auto validateLeasedInsertions = [](std::span<const EntityPageLocation> inserted,
																 std::span<const NativeGraphLogicalIdLeaseRange> ranges,
																 uint64_t currentHighWater, const char *entityName) {
			if (inserted.empty()) {
				return;
			}
			// Lower typed-store callers that do not use the graph planner retain the
			// original contiguous high-water reservation path. They can never insert a
			// stale ID below the current high-water mark; planner transactions always
			// provide durable ranges to support later rebases.
			if (ranges.empty()) {
				if (currentHighWater == 0U ||
					std::ranges::any_of(inserted, [currentHighWater](const EntityPageLocation &location) {
						return location.entity_id < currentHighWater;
					})) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  std::string("native graph ") + entityName +
												  " insertion lies below the current logical-ID high-water mark");
				}
				return;
			}
			if (std::ranges::any_of(inserted, [&ranges](const EntityPageLocation &location) {
					return !std::ranges::any_of(ranges, [&location](const NativeGraphLogicalIdLeaseRange &range) {
						return range.contains(location.entity_id);
					});
				})) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  std::string("native graph ") + entityName +
											  " insertion lies outside its durable logical-ID lease");
			}
		};
		validateLeaseRanges(logicalIdReservation.node_ranges, current.next_logical_node_id, "Node");
		validateLeaseRanges(logicalIdReservation.edge_ranges, current.next_logical_edge_id, "Edge");
		validateLeasedInsertions(insertedNodes, logicalIdReservation.node_ranges, current.next_logical_node_id, "Node");
		validateLeasedInsertions(insertedEdges, logicalIdReservation.edge_ranges, current.next_logical_edge_id, "Edge");
		// Entity-keyed Property roots are part of an Entity's logical lifetime,
		// not a second independently visible object. Remove their directory entries
		// with the Entity Map deletion so a new snapshot cannot resolve Properties
		// for an Entity that is already absent. The old Property-directory root
		// remains reachable through existing snapshot pins until normal reclamation.
		std::vector<EntityPropertyKey> erasedEntityProperties;
		erasedEntityProperties.reserve(erasedNodes.size() + erasedEdges.size());
		std::set<uint64_t> explicitlyErasedEntityPropertyIds;
		for (const EntityPropertyKey &key: explicitlyErasedEntityProperties) {
			if (!explicitlyErasedEntityPropertyIds.insert(key.directoryId()).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction has duplicate explicit Entity Property deletion");
			}
		}
		const auto collectErasedEntityProperties = [this, &current, &touchedPropertyKeys, &erasedEntityProperties,
													&explicitlyErasedEntityPropertyIds](
														   std::span<const uint64_t> entityIds,
														   EntityPropertyOwnerKind ownerKind) {
			for (const uint64_t entityId: entityIds) {
				const EntityPropertyKey key{ownerKind, entityId};
				if (key.isValid() && locations_.findEntityPropertyOptional(current, key).has_value()) {
					if (explicitlyErasedEntityPropertyIds.contains(key.directoryId())) {
						// The matching Property-page COW has already claimed this
						// directory erase, so do not emit a duplicate deletion.
						continue;
					}
					if (!touchedPropertyKeys.insert(key.directoryId()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph page transaction has overlapping Entity Property deletion");
					}
					erasedEntityProperties.push_back(key);
				}
			}
		};
		collectErasedEntityProperties(erasedNodes, EntityPropertyOwnerKind::EPOK_NODE);
		collectErasedEntityProperties(erasedEdges, EntityPropertyOwnerKind::EPOK_EDGE);
		std::ranges::sort(erasedEntityProperties, {}, &EntityPropertyKey::directoryId);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: insertedNodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: insertedEdges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		for (const uint64_t pageId: pageIds) {
			batch.observeLogicalPageId(PageId(pageId));
		}
		std::set<uint64_t> reservedNewPoolSlots;
		std::vector<CatalogEntry> newPoolEntries;
		newPoolEntries.reserve(newPoolRequests.size() + freshReplacementPools.size());
		std::map<uint64_t, size_t> newPoolEntryBySegment;
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto fresh = freshReplacementPools.find(request.reservation.segment_id.value());
			if (fresh != freshReplacementPools.end() && !hasSameFreshEntityPoolIdentity(fresh->second, request)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page transaction conflicts with its fresh COW-pool reservation");
			}
		}
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto existing = newPoolEntryBySegment.find(request.reservation.segment_id.value());
			if (existing != newPoolEntryBySegment.end()) {
				const CatalogEntry &entry = newPoolEntries[existing->second];
				newPoolMaterializations.push_back(
						{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
				continue;
			}

			PhysicalSlotId candidate = request.first_slot;
			std::optional<PhysicalSlotId> physicalSlot;
			while (candidate <= request.last_slot) {
				physicalSlot = allocator_.findFirstFree(current, candidate, request.last_slot);
				if (!physicalSlot.has_value() || reservedNewPoolSlots.insert(physicalSlot->value()).second) {
					break;
				}
				if (*physicalSlot == request.last_slot ||
					physicalSlot->value() == std::numeric_limits<uint64_t>::max()) {
					physicalSlot.reset();
					break;
				}
				candidate = PhysicalSlotId(physicalSlot->value() + 1U);
			}
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool batch has no free physical slot in its request range");
			}
			CatalogEntry entry;
			entry.segment_id = request.reservation.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = request.reservation.kind;
			entry.owner_id = request.reservation.owner_id;
			entry.generation = request.reservation.segment_generation;
			entry.state = request.reservation.state;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(entry.segment_id.value(), newPoolEntries.size());
			newPoolEntries.push_back(entry);
			newPoolMaterializations.push_back(
					{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
		}
		for (const auto &[segmentId, destination]: freshReplacementPools) {
			if (newPoolEntryBySegment.contains(segmentId)) {
				continue;
			}
			PhysicalSlotId candidate = destination.first_slot;
			std::optional<PhysicalSlotId> physicalSlot;
			while (candidate <= destination.last_slot) {
				physicalSlot = allocator_.findFirstFree(current, candidate, destination.last_slot);
				if (!physicalSlot.has_value() || reservedNewPoolSlots.insert(physicalSlot->value()).second) {
					break;
				}
				if (*physicalSlot == destination.last_slot ||
					physicalSlot->value() == std::numeric_limits<uint64_t>::max()) {
					physicalSlot.reset();
					break;
				}
				candidate = PhysicalSlotId(physicalSlot->value() + 1U);
			}
			if (!physicalSlot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination has no free physical slot");
			}
			CatalogEntry entry;
			entry.segment_id = destination.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			entry.owner_id = destination.pool_owner_id;
			entry.generation = destination.generation;
			entry.state = SegmentState::SGS_SEALED;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(segmentId, newPoolEntries.size());
			newPoolEntries.push_back(entry);
		}
		for (size_t ordinal = 0U; ordinal < replacementFreshDestinations.size(); ++ordinal) {
			if (!replacementFreshDestinations[ordinal].has_value()) {
				continue;
			}
			const EntityPageFreshPoolDestination &destination = *replacementFreshDestinations[ordinal];
			const auto entryOrdinal = newPoolEntryBySegment.find(destination.segment_id.value());
			if (entryOrdinal == newPoolEntryBySegment.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination was not materialized");
			}
			const CatalogEntry &entry = newPoolEntries[entryOrdinal->second];
			EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			materialization.pool = entry;
			materialization.replacement = {materialization.expected_location.page_id, entry.segment_id, 0U,
										   entry.generation, 0U};
		}
		std::vector<CatalogEntry> newPropertyPoolEntries;
		newPropertyPoolEntries.reserve(propertyPageBatchRequests.size());
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			CatalogEntry entry = request.expected_pool;
			if (request.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL) {
				PhysicalSlotId candidate = request.first_slot;
				std::optional<PhysicalSlotId> physicalSlot;
				while (candidate <= request.last_slot) {
					physicalSlot = allocator_.findFirstFree(current, candidate, request.last_slot);
					if (!physicalSlot.has_value() || reservedNewPoolSlots.insert(physicalSlot->value()).second) {
						break;
					}
					if (*physicalSlot == request.last_slot ||
						physicalSlot->value() == std::numeric_limits<uint64_t>::max()) {
						physicalSlot.reset();
						break;
					}
					candidate = PhysicalSlotId(physicalSlot->value() + 1U);
				}
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation has no free physical slot in its request range");
				}
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = *physicalSlot;
				entry.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
				entry.owner_id = request.page_owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, entry.segment_id).has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation allocated a published logical Segment ID");
				}
				batch.reserveDataSlot(entry.physical_slot);
				newPropertyPoolEntries.push_back(entry);
			}

			std::vector<NativeGraphPropertyPageInitialMaterialization> initialPages;
			initialPages.reserve(request.page_property_keys.size());
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value() ||
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property page batch allocated a published logical Page ID");
				}
				std::vector<EntityPropertyLocation> properties;
				properties.reserve(pageKeys.size());
				for (const EntityPropertyKey &key: pageKeys) {
					properties.push_back({key, pageId, std::numeric_limits<uint16_t>::max(), 0U});
				}
				initialPages.push_back(
						{{pageId, entry.segment_id, 0U, entry.generation, 0U}, std::move(properties)});
			}
			propertyPageBatchMaterializations.push_back(
					{request.kind, entry, request.page_owner_id, std::move(initialPages)});
		}
		const auto reserveDistinctNewPoolSlot = [this, &current, &reservedNewPoolSlots](PhysicalSlotId first,
																						PhysicalSlotId last,
																						const char *message) {
			PhysicalSlotId candidate = first;
			while (candidate <= last) {
				const std::optional<PhysicalSlotId> found = allocator_.findFirstFree(current, candidate, last);
				if (!found.has_value()) {
					break;
				}
				if (reservedNewPoolSlots.insert(found->value()).second) {
					return *found;
				}
				if (*found == last || found->value() == std::numeric_limits<uint64_t>::max()) {
					break;
				}
				candidate = PhysicalSlotId(found->value() + 1U);
			}
			throw FormatException(FormatError::FERR_INVALID_STATE, message);
		};
		struct SharedLargeValuePoolCapacity {
			CatalogEntry pool;
			size_t available_frames = 0U;
			size_t prepared_frames = 0U;
			bool is_new_pool = false;
		};
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueDataPools;
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueManifestPools;
		if (!largeValues.empty()) {
			// Publication owns the writer lock, so a regular snapshot acquisition would
			// self-deadlock. This pin reads the already fenced root without reacquiring
			// the reader lock; the header checks below remain fail-closed.
			MetadataSnapshotPin poolSnapshot = snapshots_.pinCurrentUnderWrite(writer, current);
			for (const CatalogEntry &pool: findCurrentLargeValueDataPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue capacity query found an unallocated pool slot");
				}
				sharedLargeValueDataPools.push_back({pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
															   *io_, current.data_region_offset, pool)});
			}
			for (const CatalogEntry &pool: findCurrentLargeValueManifestPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue Manifest capacity query found an unallocated pool slot");
				}
				sharedLargeValueManifestPools.push_back(
						{pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
								   *io_, current.data_region_offset, pool)});
			}
			poolSnapshot.release();
		}
		std::vector<CatalogEntry> newLargeValuePoolEntries;
		const auto allocateSharedLargeValuePages = [this, &batch, &current, &pageIds](
				std::vector<SharedLargeValuePoolCapacity> &candidates, size_t &remainingPages,
				std::vector<LargeValueDataMaterializationPoolBatch> &destination, const char *duplicatePageMessage) {
			for (SharedLargeValuePoolCapacity &candidate: candidates) {
				if (remainingPages == 0U) {
					break;
				}
				if (candidate.available_frames == 0U) {
					continue;
				}
				const size_t pages = std::min(remainingPages, candidate.available_frames);
				const size_t preparedPrefix = candidate.is_new_pool ? candidate.prepared_frames : 0U;
				LargeValueDataMaterializationPoolBatch poolBatch{
						candidate.pool, {}, {}, candidate.is_new_pool, preparedPrefix};
				poolBatch.page_ids.reserve(pages);
				poolBatch.locations.reserve(pages);
				for (size_t pageOrdinal = 0U; pageOrdinal < pages; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() ||
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE, duplicatePageMessage);
					}
					poolBatch.page_ids.push_back(pageId);
					poolBatch.locations.push_back(
							{pageId, candidate.pool.segment_id, 0U, candidate.pool.generation, 0U});
				}
				destination.push_back(std::move(poolBatch));
				candidate.available_frames -= pages;
				if (candidate.is_new_pool) {
					candidate.prepared_frames += pages;
				}
				remainingPages -= pages;
			}
		};
		for (const NativeGraphLargeValueMutation &mutation: largeValues) {
			LargeValueMaterializationResult materialization;
			materialization.value_id = batch.allocateLargeValueId();
			if (materialization.value_id != mutation.reference.value_id) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue allocation does not match its pinned logical value ID");
			}
			size_t remainingDataPages = mutation.planned_data_page_count;
			materialization.data_pools.reserve((remainingDataPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
											   RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
					"native graph LargeValue allocation reused a published shared-pool data Page ID");
			while (remainingDataPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_data_slot, mutation.last_data_slot,
						"native graph LargeValue materialization has no free data-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published logical Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueDataPools.push_back(
						{pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(
						sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
						"native graph LargeValue allocation reused a published logical data Page ID");
			}
			size_t remainingManifestPages = mutation.planned_manifest_page_count;
			materialization.manifest_pools.reserve(
					(remainingManifestPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
					RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueManifestPools, remainingManifestPages, materialization.manifest_pools,
					"native graph LargeValue allocation reused a published shared-pool Manifest Page ID");
			while (remainingManifestPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_manifest_slot, mutation.last_manifest_slot,
						"native graph LargeValue materialization has no free manifest-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published manifest Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueManifestPools.push_back(
						{pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(
						sharedLargeValueManifestPools, remainingManifestPages, materialization.manifest_pools,
						"native graph LargeValue allocation reused a published manifest Page ID");
			}
			materialization.manifest_pool = materialization.manifest_pools.front().pool;
			materialization.manifest_page_id = materialization.manifest_pools.front().page_ids.front();
			materialization.manifest_location = materialization.manifest_pools.front().locations.front();
			largeValueMaterializations.push_back(std::move(materialization));
		}
		// Allocate every fresh derived Index pool from this same candidate batch.
		// Its logical Segment/Page IDs and physical slot reservation therefore have
		// exactly the same visibility boundary as the Node, Edge, and Property work.
		std::vector<CatalogEntry> newDerivedIndexPoolEntries;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < derivedState.index_tree_mutations.size();
			 ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = derivedState.index_tree_mutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (const NewIndexTreePoolRequest &request: mutation.new_pool_requests) {
				PhysicalSlotId candidate = request.first_slot;
				std::optional<PhysicalSlotId> physicalSlot;
				while (candidate <= request.last_slot) {
					physicalSlot = allocator_.findFirstFree(current, candidate, request.last_slot);
					if (!physicalSlot.has_value() || reservedNewPoolSlots.insert(physicalSlot->value()).second) {
						break;
					}
					if (*physicalSlot == request.last_slot ||
						physicalSlot->value() == std::numeric_limits<uint64_t>::max()) {
						physicalSlot.reset();
						break;
					}
					candidate = PhysicalSlotId(physicalSlot->value() + 1U);
				}
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state mutation has no free physical slot for a new Index pool");
				}
				const SegmentId segmentId = batch.allocateLogicalSegmentId();
				if (catalog_.findOptional(current, segmentId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation allocated a published Index Segment ID");
				}
				CatalogEntry pool;
				pool.segment_id = segmentId;
				pool.physical_slot = *physicalSlot;
				pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
				pool.owner_id = request.owner_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				batch.reserveDataSlot(pool.physical_slot);

				IndexTreeMutationPoolBatch materialization;
				materialization.pool = pool;
				materialization.newly_reserved = true;
				materialization.appended_page_ids.reserve(request.appended_page_count);
				materialization.appended_locations.reserve(request.appended_page_count);
				for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() ||
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published Index Page ID");
					}
					materialization.appended_page_ids.push_back(pageId);
					materialization.appended_locations.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
				}
				newDerivedIndexPoolEntries.push_back(pool);
				batches.push_back(std::move(materialization));
			}
		}
		std::vector<NativeGraphVectorBootstrapMaterialization> vectorBootstrapMaterializations;
		std::vector<CatalogEntry> newVectorBootstrapPoolEntries;
		vectorBootstrapMaterializations.reserve(derivedState.vector_bootstraps.size());
		const auto resolveFreshDerivedRoot = [&derivedState, &derivedIndexBatches](uint64_t indexId) {
			const auto mutation = std::ranges::find(derivedState.index_tree_mutations, indexId,
													&NativeGraphIndexTreeMutation::index_id);
			if (mutation == derivedState.index_tree_mutations.end() || !mutation->replacement_root.has_value() ||
				!mutation->replacement_root->newly_reserved) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap lost a freshly allocated mapping root");
			}
			const size_t mutationOrdinal = static_cast<size_t>(mutation - derivedState.index_tree_mutations.begin());
			const NativeGraphIndexTreeAppendedPageReference &reference = *mutation->replacement_root;
			const size_t poolOrdinal = mutation->pool_requests.size() + reference.pool_ordinal;
			if (mutationOrdinal >= derivedIndexBatches.size() ||
				poolOrdinal >= derivedIndexBatches[mutationOrdinal].size() ||
				reference.appended_page_ordinal >=
						derivedIndexBatches[mutationOrdinal][poolOrdinal].appended_locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector bootstrap has an invalid freshly allocated mapping-root location");
			}
			const PageLocation &location = derivedIndexBatches[mutationOrdinal][poolOrdinal]
												   .appended_locations[reference.appended_page_ordinal];
			return IndexRootLocation{indexId, location.page_id, location.generation};
		};
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			NativeGraphVectorBootstrapMaterialization materialization;
			materialization.mutation = mutation;
			materialization.node_mapping_root = resolveFreshDerivedRoot(mutation.index_id);
			materialization.vector_node_mapping_root = resolveFreshDerivedRoot(mutation.vector_node_mapping_index_id);
			for (uint64_t offset = 0U; offset < mutation.vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation.first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap lost its reserved logical Vector-ID range");
				}
			}
			const size_t rawPoolCount =
					(static_cast<size_t>(mutation.raw_page_count) - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
			materialization.raw_pools.reserve(rawPoolCount);
			materialization.raw_pages.reserve(mutation.raw_page_count);
			for (size_t poolOrdinal = 0U; poolOrdinal < rawPoolCount; ++poolOrdinal) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector bootstrap has no free physical slot for a Raw pool");
				pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
				pool.owner_id = mutation.index_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw-pool Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				materialization.raw_pools.push_back(pool);
				newVectorBootstrapPoolEntries.push_back(pool);
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < mutation.raw_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value() ||
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw Page ID");
				}
				const CatalogEntry &pool = materialization.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				materialization.raw_pages.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
			}
			materialization.manifest_pool.segment_id = batch.allocateLogicalSegmentId();
			materialization.manifest_pool.physical_slot = reserveDistinctNewPoolSlot(
					mutation.first_slot, mutation.last_slot,
					"native graph Vector bootstrap has no free physical slot for a Manifest pool");
			materialization.manifest_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
			materialization.manifest_pool.owner_id = mutation.index_id;
			materialization.manifest_pool.generation = Generation(1U);
			materialization.manifest_pool.state = SegmentState::SGS_SEALED;
			materialization.manifest_pool.committed_lsn = checkpointLsn;
			if (catalog_.findOptional(current, materialization.manifest_pool.segment_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest-pool Segment ID");
			}
			batch.reserveDataSlot(materialization.manifest_pool.physical_slot);
			materialization.manifest_page.page_id = batch.allocateLogicalPageId();
			materialization.manifest_page.segment_id = materialization.manifest_pool.segment_id;
			materialization.manifest_page.generation = materialization.manifest_pool.generation;
			if (locations_.findPageOptional(current, materialization.manifest_page.page_id).has_value() ||
				!pageIds.insert(materialization.manifest_page.page_id.value()).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest Page ID");
			}
			materialization.manifest_location = {mutation.index_id, materialization.manifest_page.page_id,
												 materialization.manifest_page.generation};
			newVectorBootstrapPoolEntries.push_back(materialization.manifest_pool);
			vectorBootstrapMaterializations.push_back(std::move(materialization));
		}
		for (const NativeGraphVectorDeltaMutation *mutation: vectorDeltasByVectorRange) {
			for (uint64_t offset = 0U; offset < mutation->inserted_vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation->first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta lost its reserved logical Vector-ID range");
				}
			}
		}
		std::vector<NativeGraphVectorDeltaMaterialization> vectorDeltaMaterializations;
		std::vector<CatalogEntry> newVectorDeltaPoolEntries;
		vectorDeltaMaterializations.reserve(derivedState.vector_deltas.size());
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			NativeGraphVectorDeltaMaterialization materialization;
			materialization.mutation = mutation;
			std::map<uint32_t, size_t> rawNewPoolOrdinals;
			for (const NativeGraphVectorDeltaRawPageMutation &rawMutation: mutation.raw_pages) {
				NativeGraphVectorDeltaRawPageMaterialization raw;
				raw.mutation = rawMutation;
				if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL) {
					raw.pool = rawMutation.expected_pool;
					raw.location = rawMutation.expected_location;
				} else if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL) {
					raw.pool = rawMutation.expected_pool;
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() ||
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				} else {
					auto [position, inserted] = rawNewPoolOrdinals.try_emplace(rawMutation.new_pool_ordinal,
																			   materialization.raw_new_pools.size());
					if (inserted) {
						CatalogEntry pool;
						pool.segment_id = batch.allocateLogicalSegmentId();
						pool.physical_slot = reserveDistinctNewPoolSlot(
								mutation.first_slot, mutation.last_slot,
								"native graph Vector delta has no free physical slot for a Raw pool");
						pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
						pool.owner_id = mutation.index_id;
						pool.generation = Generation(1U);
						pool.state = SegmentState::SGS_SEALED;
						pool.committed_lsn = checkpointLsn;
						if (catalog_.findOptional(current, pool.segment_id).has_value()) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph Vector delta allocated a published Raw-pool Segment ID");
						}
						batch.reserveDataSlot(pool.physical_slot);
						newVectorDeltaPoolEntries.push_back(pool);
						materialization.raw_new_pools.push_back(pool);
					}
					raw.pool = materialization.raw_new_pools[position->second];
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() ||
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published fresh Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				}
				materialization.raw_pages.push_back(std::move(raw));
			}
			if (mutation.manifest_in_new_pool) {
				materialization.manifest_new_pool.segment_id = batch.allocateLogicalSegmentId();
				materialization.manifest_new_pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector delta has no free physical slot for a Manifest pool");
				materialization.manifest_new_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
				materialization.manifest_new_pool.owner_id = mutation.index_id;
				materialization.manifest_new_pool.generation = Generation(1U);
				materialization.manifest_new_pool.state = SegmentState::SGS_SEALED;
				materialization.manifest_new_pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, materialization.manifest_new_pool.segment_id).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta allocated a published Manifest-pool Segment ID");
				}
				batch.reserveDataSlot(materialization.manifest_new_pool.physical_slot);
				newVectorDeltaPoolEntries.push_back(materialization.manifest_new_pool);
			}
			const CatalogEntry &manifestPool =
					mutation.manifest_in_new_pool ? materialization.manifest_new_pool : mutation.expected_manifest_pool;
			materialization.manifest_replacement = {mutation.expected_manifest_location.page_id,
													manifestPool.segment_id, 0U, manifestPool.generation, 0U};
			materialization.manifest_location = {mutation.index_id, materialization.manifest_replacement.page_id,
												 materialization.manifest_replacement.generation};
			vectorDeltaMaterializations.push_back(std::move(materialization));
		}
		std::vector<CatalogEntry> allNewPoolEntries;
		allNewPoolEntries.reserve(newPoolEntries.size() + newPropertyPoolEntries.size() +
								  newLargeValuePoolEntries.size() + newDerivedIndexPoolEntries.size() +
								  newVectorBootstrapPoolEntries.size() + newVectorDeltaPoolEntries.size());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPoolEntries.begin(), newPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPropertyPoolEntries.begin(), newPropertyPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newLargeValuePoolEntries.begin(),
								 newLargeValuePoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newDerivedIndexPoolEntries.begin(),
								 newDerivedIndexPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorBootstrapPoolEntries.begin(),
								 newVectorBootstrapPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorDeltaPoolEntries.begin(),
								 newVectorDeltaPoolEntries.end());
		for (const CatalogEntry &entry: allNewPoolEntries) {
			allocator_.prepareSetAllocated(batch, entry.physical_slot, true);
		}
		if (!allNewPoolEntries.empty()) {
			std::ranges::sort(allNewPoolEntries, {}, &CatalogEntry::segment_id);
			catalog_.prepareUpsert(batch, allNewPoolEntries);
		}
		for (size_t mutationOrdinal = 0U; mutationOrdinal < derivedState.index_tree_mutations.size();
			 ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = derivedState.index_tree_mutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (size_t poolOrdinal = 0U; poolOrdinal < mutation.pool_requests.size(); ++poolOrdinal) {
				IndexTreeMutationPoolBatch &pool = batches[poolOrdinal];
				const size_t appendedCount = mutation.pool_requests[poolOrdinal].appended_page_count;
				pool.appended_page_ids.reserve(appendedCount);
				pool.appended_locations.reserve(appendedCount);
				for (size_t pageOrdinal = 0U; pageOrdinal < appendedCount; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() ||
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published appended Index Page ID");
					}
					pool.appended_page_ids.push_back(pageId);
					pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
				}
			}
		}

		const std::vector<EntityPageReplacementMaterialization> plannedMaterializations = materializations;
		const std::vector<EntityPageAppendBatchMaterialization> plannedAppendMaterializations = appendMaterializations;
		const std::vector<EntityPageNewPoolBatchMaterialization> plannedNewPoolMaterializations =
				newPoolMaterializations;
		const std::vector<NativeGraphPropertyPageReplacementMaterialization> plannedPropertyMaterializations =
				propertyMaterializations;
		const std::vector<NativeGraphPropertyPageBatchMaterialization> plannedPropertyPageBatchMaterializations =
				propertyPageBatchMaterializations;
		const std::vector<LargeValueMaterializationResult> plannedLargeValueMaterializations =
				largeValueMaterializations;
		const std::vector<std::vector<IndexTreeMutationPoolBatch>> plannedDerivedIndexBatches = derivedIndexBatches;
		const std::vector<NativeGraphVectorBootstrapMaterialization> plannedVectorBootstrapMaterializations =
				vectorBootstrapMaterializations;
		if (hasLargeValueRetirements) {
			largeValueRetirementPreparer(largeValueRetirementBatches);
		}
		preparer(materializations, appendMaterializations, newPoolMaterializations, propertyMaterializations,
				 propertyPageBatchMaterializations, largeValueMaterializations);
		if (!derivedState.empty()) {
			derivedStatePreparer(derivedIndexBatches);
		}
		if (!vectorBootstrapMaterializations.empty()) {
			vectorBootstrapPreparer(vectorBootstrapMaterializations);
		}
		std::set<std::pair<uint64_t, uint8_t>> destinationFrames;
		for (size_t ordinal = 0U; ordinal < materializations.size(); ++ordinal) {
			const EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			const EntityPageReplacementMaterialization &planned = plannedMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.destination_is_new_pool != planned.destination_is_new_pool) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind));
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable Entity page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
				continue;
			}
			const uint8_t invalidReplacement =
					static_cast<uint8_t>(materialization.replacement.page_id !=
										 materialization.expected_location.page_id) |
					static_cast<uint8_t>(materialization.replacement.segment_id != materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.replacement.generation != materialization.pool.generation) |
					static_cast<uint8_t>(materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(
							materialization.replacement.segment_id == materialization.expected_location.segment_id &&
							materialization.replacement.page_index == materialization.expected_location.page_index) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.replacement.page_index)
												  .second);
			if (invalidReplacement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < appendMaterializations.size(); ++ordinal) {
			const EntityPageAppendBatchMaterialization &materialization = appendMaterializations[ordinal];
			const EntityPageAppendBatchMaterialization &planned = plannedAppendMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.appended_location.page_id !=
										 planned.appended_location.page_id) |
					static_cast<uint8_t>(materialization.appended_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.appended_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.appended_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.appended_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.appended_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch append callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < newPoolMaterializations.size(); ++ordinal) {
			const EntityPageNewPoolBatchMaterialization &materialization = newPoolMaterializations[ordinal];
			const EntityPageNewPoolBatchMaterialization &planned = plannedNewPoolMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.initial_location.page_id != planned.initial_location.page_id) |
					static_cast<uint8_t>(materialization.initial_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.initial_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.initial_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.initial_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.initial_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool callback returned an invalid initial page frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < propertyMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageReplacementMaterialization &materialization =
					propertyMaterializations[ordinal];
			const NativeGraphPropertyPageReplacementMaterialization &planned = plannedPropertyMaterializations[ordinal];
			const bool invalidReplacement =
					materialization.replacement.page_id != materialization.expected_location.page_id ||
					materialization.replacement.segment_id != materialization.pool.segment_id ||
					materialization.replacement.generation != materialization.pool.generation ||
					materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME ||
					materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
					materialization.replacement.page_index == materialization.expected_location.page_index ||
					!destinationFrames
							 .emplace(materialization.pool.segment_id.value(), materialization.replacement.page_index)
							 .second;
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.expected_owner_lsn != planned.expected_owner_lsn) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(!materialization.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!materialization.retire_page && invalidReplacement);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction callback returned an invalid Property page frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
			}
		}
		for (size_t ordinal = 0U; ordinal < propertyPageBatchMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageBatchMaterialization &materialization =
					propertyPageBatchMaterializations[ordinal];
			const NativeGraphPropertyPageBatchMaterialization &planned =
					plannedPropertyPageBatchMaterializations[ordinal];
			const uint8_t invalidPool =
					static_cast<uint8_t>(materialization.kind != planned.kind) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.pool, planned.pool)) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(materialization.pages.size() != planned.pages.size()) |
					static_cast<uint8_t>(materialization.pages.empty()) |
					static_cast<uint8_t>(materialization.pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph page transaction callback changed a Property page-batch identity");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < materialization.pages.size(); ++pageOrdinal) {
				const NativeGraphPropertyPageInitialMaterialization &page = materialization.pages[pageOrdinal];
				const NativeGraphPropertyPageInitialMaterialization &expectedPage = planned.pages[pageOrdinal];
				const uint8_t invalidPage =
						static_cast<uint8_t>(page.location.page_id != expectedPage.location.page_id) |
						static_cast<uint8_t>(page.location.segment_id != materialization.pool.segment_id) |
						static_cast<uint8_t>(page.location.generation != materialization.pool.generation) |
						static_cast<uint8_t>(page.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(page.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(page.location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(page.upserted_properties.size() !=
										 expectedPage.upserted_properties.size()) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(materialization.pool.segment_id.value(),
														 page.location.page_index)
									 .second);
				if (invalidPage != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction callback returned an invalid Property page frame");
				}
				for (size_t propertyOrdinal = 0U; propertyOrdinal < page.upserted_properties.size();
					 ++propertyOrdinal) {
					const EntityPropertyLocation &property = page.upserted_properties[propertyOrdinal];
					const EntityPropertyLocation &expected = expectedPage.upserted_properties[propertyOrdinal];
					const uint8_t invalidProperty =
							static_cast<uint8_t>(property.key != expected.key) |
							static_cast<uint8_t>(property.page_id != page.location.page_id) |
							static_cast<uint8_t>(property.slot_id == std::numeric_limits<uint16_t>::max()) |
							static_cast<uint8_t>(property.slot_generation == 0U) |
							static_cast<uint8_t>(!publishedPropertySlots
											  .emplace(property.page_id.value(), property.slot_id)
											  .second);
					if (invalidProperty != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph Property page callback returned an invalid directory location");
					}
					upsertedEntityProperties.push_back(property);
				}
			}
		}
		std::vector<PageLocation> publishedLargeValuePages;
		std::vector<LargeValueManifestLocation> publishedLargeValueManifests;
		for (size_t ordinal = 0U; ordinal < largeValueMaterializations.size(); ++ordinal) {
			const LargeValueMaterializationResult &materialization = largeValueMaterializations[ordinal];
			const LargeValueMaterializationResult &planned = plannedLargeValueMaterializations[ordinal];
			const NativeGraphLargeValueMutation &mutation = largeValues[ordinal];
			const bool hasManifestPool = !materialization.manifest_pools.empty();
			const uint8_t invalidManifest =
					static_cast<uint8_t>(materialization.value_id != planned.value_id) |
					static_cast<uint8_t>(materialization.value_id != mutation.reference.value_id) |
					static_cast<uint8_t>(materialization.data_pools.size() != planned.data_pools.size()) |
					static_cast<uint8_t>(materialization.manifest_pools.size() != planned.manifest_pools.size()) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, planned.manifest_pool)) |
					static_cast<uint8_t>(materialization.manifest_page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.segment_id !=
										 materialization.manifest_pool.segment_id) |
					static_cast<uint8_t>(materialization.manifest_location.generation !=
										 mutation.reference.generation) |
					static_cast<uint8_t>(materialization.manifest_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.manifest_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(materialization.manifest_location.free_bytes != 0U) |
					static_cast<uint8_t>(!hasManifestPool);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest identity or frame");
			}
			const LargeValueManifestMaterializationPoolBatch &firstManifestPool =
					materialization.manifest_pools.front();
			const uint8_t invalidManifestRoot =
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, firstManifestPool.pool)) |
					static_cast<uint8_t>(firstManifestPool.page_ids.empty()) |
					static_cast<uint8_t>(firstManifestPool.locations.empty()) |
					static_cast<uint8_t>(firstManifestPool.page_ids.front() != materialization.manifest_page_id) |
					static_cast<uint8_t>(firstManifestPool.locations.front() != materialization.manifest_location);
			if (invalidManifestRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest root frame");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.data_pools.size(); ++poolOrdinal) {
				const LargeValueDataMaterializationPoolBatch &pool = materialization.data_pools[poolOrdinal];
				const LargeValueDataMaterializationPoolBatch &expected = planned.data_pools[poolOrdinal];
				const uint8_t invalidPool = static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
											static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
											static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
											static_cast<uint8_t>(pool.prepared_prefix_count !=
															 expected.prepared_prefix_count) |
											static_cast<uint8_t>(!pool.is_new_pool &&
															 pool.prepared_prefix_count != 0U) |
											static_cast<uint8_t>(pool.page_ids.size() >
															 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(pool.is_new_pool &&
															 (pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
															  pool.page_ids.size() >
																	  RSS_PAGE_POOL_DATA_FRAME_COUNT -
																			  std::min(pool.prepared_prefix_count,
																					   static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
											static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue preparer changed a data-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
								static_cast<uint8_t>(location.free_bytes != 0U) |
								static_cast<uint8_t>(pool.is_new_pool &&
													 location.page_index !=
															 pool.prepared_prefix_count + pageOrdinal + 1U) |
								static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate data frame");
					}
					publishedLargeValuePages.push_back(location);
				}
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.manifest_pools.size(); ++poolOrdinal) {
				const LargeValueManifestMaterializationPoolBatch &pool = materialization.manifest_pools[poolOrdinal];
				const LargeValueManifestMaterializationPoolBatch &expected = planned.manifest_pools[poolOrdinal];
				const uint8_t invalidPool = static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
											static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
											static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
											static_cast<uint8_t>(pool.prepared_prefix_count !=
															 expected.prepared_prefix_count) |
											static_cast<uint8_t>(!pool.is_new_pool &&
															 pool.prepared_prefix_count != 0U) |
											static_cast<uint8_t>(pool.page_ids.size() >
															 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(pool.is_new_pool &&
															 (pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
															  pool.page_ids.size() >
																	  RSS_PAGE_POOL_DATA_FRAME_COUNT -
																			  std::min(pool.prepared_prefix_count,
																					   static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
											static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue preparer changed a manifest-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
								static_cast<uint8_t>(location.free_bytes != 0U) |
								static_cast<uint8_t>(pool.is_new_pool &&
													 location.page_index !=
															 pool.prepared_prefix_count + pageOrdinal + 1U) |
								static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate manifest frame");
					}
					publishedLargeValuePages.push_back(location);
				}
			}
			publishedLargeValueManifests.push_back({mutation.reference.value_id, materialization.manifest_page_id,
													materialization.manifest_pool.generation});
		}

		std::vector<PageLocation> publishedDerivedIndexPages;
		std::vector<IndexRootLocation> publishedDerivedIndexRoots;
		std::vector<uint64_t> erasedDerivedIndexRootIds;
		publishedDerivedIndexRoots.reserve(derivedState.index_tree_mutations.size());
		erasedDerivedIndexRootIds.reserve(derivedState.index_tree_mutations.size());
		for (size_t mutationOrdinal = 0U; mutationOrdinal < derivedState.index_tree_mutations.size();
			 ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = derivedState.index_tree_mutations[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &planned = plannedDerivedIndexBatches[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &materialized = derivedIndexBatches[mutationOrdinal];
			if (materialized.size() != planned.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph derived-state preparer changed its Index pool batch cardinality");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialized.size(); ++poolOrdinal) {
				const IndexTreeMutationPoolBatch &expected = planned[poolOrdinal];
				const IndexTreeMutationPoolBatch &actual = materialized[poolOrdinal];
				const bool invalidShape =
						actual.pool.segment_id != expected.pool.segment_id ||
						actual.pool.physical_slot != expected.pool.physical_slot ||
						actual.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
						actual.pool.kind != expected.pool.kind || actual.pool.owner_id != expected.pool.owner_id ||
						actual.pool.generation != expected.pool.generation ||
						actual.pool.state != SegmentState::SGS_SEALED || actual.pool.state != expected.pool.state ||
						actual.pool.committed_lsn != expected.pool.committed_lsn ||
						actual.expected_replacements != expected.expected_replacements ||
						actual.appended_page_ids != expected.appended_page_ids ||
						actual.expected_retirements != expected.expected_retirements ||
						actual.newly_reserved != expected.newly_reserved ||
						actual.prepared_new_pool_prefix_count != expected.prepared_new_pool_prefix_count ||
						actual.replacement_locations.size() != expected.replacement_locations.size() ||
						actual.appended_locations.size() != expected.appended_locations.size();
				if (invalidShape) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state preparer changed an Index pool identity or page shape");
				}
				const auto validatePublishedLocation = [&destinationFrames, &publishedDerivedIndexPages,
														&actual](const PageLocation &location, PageId expectedPageId) {
					const uint8_t invalid =
							static_cast<uint8_t>(location.page_id != expectedPageId) |
							static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
							static_cast<uint8_t>(location.generation != actual.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
							static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalid != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state preparer returned an invalid Index page frame");
					}
					publishedDerivedIndexPages.push_back(location);
				};
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.replacement_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.replacement_locations[pageOrdinal],
											  actual.expected_replacements[pageOrdinal].page_id);
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.appended_locations[pageOrdinal],
											  actual.appended_page_ids[pageOrdinal]);
				}
				for (const IndexTreePageRetirement &retirement: actual.expected_retirements) {
					retiredPageIds.push_back(retirement.location.page_id.value());
				}
			}
			if (mutation.retire_root) {
				erasedDerivedIndexRootIds.push_back(mutation.index_id);
				continue;
			}

			IndexRootLocation publishedRoot{mutation.index_id, {}, {}};
			if (mutation.expected_root.has_value()) {
				publishedRoot = *mutation.expected_root;
			}
			if (mutation.replacement_root.has_value()) {
				const NativeGraphIndexTreeAppendedPageReference &reference = *mutation.replacement_root;
				const size_t poolOrdinal = reference.newly_reserved
												   ? mutation.pool_requests.size() + reference.pool_ordinal
												   : reference.pool_ordinal;
				const IndexTreeMutationPoolBatch &pool = materialized[poolOrdinal];
				const PageLocation &location = pool.appended_locations[reference.appended_page_ordinal];
				publishedRoot.root_page_id = location.page_id;
				publishedRoot.root_generation = location.generation;
			} else if (mutation.stable_replacement_root.has_value()) {
				const PageId pageId = *mutation.stable_replacement_root;
				const auto published = std::ranges::find(publishedDerivedIndexPages, pageId, &PageLocation::page_id);
				const PageLocation location = published == publishedDerivedIndexPages.end()
													  ? locations_.findPage(current, pageId)
													  : *published;
				const auto newPool =
						std::ranges::find(newDerivedIndexPoolEntries, location.segment_id, &CatalogEntry::segment_id);
				const CatalogEntry &pool = newPool == newDerivedIndexPoolEntries.end()
												   ? catalog_.find(current, location.segment_id)
												   : *newPool;
				const uint8_t invalidStableRoot =
						static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(pool.owner_id == 0U) |
						static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(pool.generation != location.generation) |
						static_cast<uint8_t>(newPool == newDerivedIndexPoolEntries.end() &&
											 !allocator_.isAllocated(current, pool.physical_slot));
				if (invalidStableRoot != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation resolved an invalid stable Index root");
				}
				publishedRoot.root_page_id = pageId;
				publishedRoot.root_generation = location.generation;
			}
			publishedDerivedIndexRoots.push_back(publishedRoot);
		}

		for (NativeGraphVectorDeltaMaterialization &materialization: vectorDeltaMaterializations) {
			const auto nodeRoot = std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.index_id,
													&IndexRootLocation::index_id);
			const auto reverseRoot =
					std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.vector_node_mapping_index_id,
									  &IndexRootLocation::index_id);
			const auto deltaRoot = materialization.mutation.delta_mapping_index_id == 0U
										   ? publishedDerivedIndexRoots.end()
										   : std::ranges::find(publishedDerivedIndexRoots,
															   materialization.mutation.delta_mapping_index_id,
															   &IndexRootLocation::index_id);
			if (nodeRoot == publishedDerivedIndexRoots.end() || reverseRoot == publishedDerivedIndexRoots.end() ||
				(materialization.mutation.delta_mapping_index_id != 0U &&
				 deltaRoot == publishedDerivedIndexRoots.end())) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector delta lost a published semantic or overlay mapping-tree root");
			}
			materialization.node_mapping_root = *nodeRoot;
			materialization.vector_node_mapping_root = *reverseRoot;
			if (deltaRoot != publishedDerivedIndexRoots.end()) {
				materialization.delta_mapping_root = *deltaRoot;
			}
		}
		const std::vector<NativeGraphVectorDeltaMaterialization> plannedVectorDeltaMaterializations =
				vectorDeltaMaterializations;
		if (!vectorDeltaMaterializations.empty()) {
			vectorDeltaPreparer(vectorDeltaMaterializations);
		}

		std::vector<PageLocation> publishedVectorBootstrapPages;
		std::vector<VectorLocation> publishedVectorBootstrapLocations;
		std::vector<VectorManifestLocation> publishedVectorBootstrapManifests;
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorBootstrapSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorBootstrapMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorBootstrapMaterialization &actual = vectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMaterialization &planned =
					plannedVectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.vector_count != mutation.vector_count) |
					static_cast<uint8_t>(actual.mutation.dimensions != mutation.dimensions) |
					static_cast<uint8_t>(actual.mutation.metric != mutation.metric) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.raw_pools.size() != planned.raw_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.vector_count) |
					static_cast<uint8_t>(actual.manifest_location != planned.manifest_location);
			if (invalidIdentity != 0U || !hasSameCatalogEntry(actual.manifest_pool, planned.manifest_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_pools[poolOrdinal], planned.raw_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer changed a Raw-pool identity");
				}
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const PageLocation &location = actual.raw_pages[pageOrdinal];
				const PageLocation &expected = planned.raw_pages[pageOrdinal];
				const CatalogEntry &pool = actual.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expected.page_id) |
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(!rawPageIds.insert(location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(location.segment_id.value(), location.page_index).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Raw page frame");
				}
				publishedVectorBootstrapPages.push_back(location);
			}
			const PageLocation &manifest = actual.manifest_page;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_page.page_id) |
					static_cast<uint8_t>(manifest.segment_id != actual.manifest_pool.segment_id) |
					static_cast<uint8_t>(manifest.generation != actual.manifest_pool.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(manifest.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer returned an invalid Manifest page frame");
			}
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorBootstrapSlots.emplace(location.page_id.value(), location.slot_id)
										 .second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Vector location");
				}
				publishedVectorBootstrapLocations.push_back(location);
			}
			publishedVectorBootstrapPages.push_back(manifest);
			publishedVectorBootstrapManifests.push_back(actual.manifest_location);
		}

		std::vector<PageLocation> publishedVectorDeltaPages;
		std::vector<VectorLocation> publishedVectorDeltaLocations;
		std::vector<uint64_t> erasedVectorDeltaIds;
		std::vector<VectorManifestLocation> publishedVectorDeltaManifests;
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorDeltaSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorDeltaMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorDeltaMaterialization &actual = vectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMaterialization &planned = plannedVectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_index_id !=
										 mutation.expected_delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.delta_mapping_index_id != mutation.delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_root !=
										 mutation.expected_delta_mapping_root) |
					static_cast<uint8_t>(actual.mutation.delta_entry_count != mutation.delta_entry_count) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.inserted_vector_count != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.mutation.erased_vector_ids != mutation.erased_vector_ids) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.delta_mapping_root != planned.delta_mapping_root) |
					static_cast<uint8_t>(actual.raw_new_pools.size() != planned.raw_new_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.manifest_location.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.manifest_location.manifest_page_id !=
										 actual.manifest_replacement.page_id) |
					static_cast<uint8_t>(actual.manifest_location.generation != actual.manifest_replacement.generation);
			if (invalidIdentity != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_new_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_new_pools[poolOrdinal], planned.raw_new_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer changed a fresh Raw-pool identity");
				}
			}
			if (mutation.manifest_in_new_pool &&
				!hasSameCatalogEntry(actual.manifest_new_pool, planned.manifest_new_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a fresh Manifest-pool identity");
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const NativeGraphVectorDeltaRawPageMaterialization &raw = actual.raw_pages[pageOrdinal];
				const NativeGraphVectorDeltaRawPageMaterialization &expected = planned.raw_pages[pageOrdinal];
				const bool cow = expected.mutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
				const uint8_t invalidRaw =
						static_cast<uint8_t>(raw.mutation.placement != expected.mutation.placement) |
						static_cast<uint8_t>(raw.mutation.page_ordinal != expected.mutation.page_ordinal) |
						static_cast<uint8_t>(raw.mutation.inserted_record_count !=
											 expected.mutation.inserted_record_count) |
						static_cast<uint8_t>(!hasSameCatalogEntry(raw.pool, expected.pool)) |
						static_cast<uint8_t>(raw.location.page_id != expected.location.page_id) |
						static_cast<uint8_t>(raw.location.segment_id != raw.pool.segment_id) |
						static_cast<uint8_t>(raw.location.generation != raw.pool.generation) |
						static_cast<uint8_t>(raw.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(raw.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(cow && raw.location.page_index == expected.location.page_index) |
						static_cast<uint8_t>(!rawPageIds.insert(raw.location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(raw.location.segment_id.value(), raw.location.page_index)
										 .second);
				if (invalidRaw != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Raw page frame");
				}
				publishedVectorDeltaPages.push_back(raw.location);
			}
			const PageLocation &manifest = actual.manifest_replacement;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_replacement.page_id) |
					static_cast<uint8_t>(manifest.segment_id != planned.manifest_replacement.segment_id) |
					static_cast<uint8_t>(manifest.generation != planned.manifest_replacement.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer returned an invalid Manifest frame");
			}
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorDeltaSlots.emplace(location.page_id.value(), location.slot_id).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Vector Location");
				}
				publishedVectorDeltaLocations.push_back(location);
			}
			publishedVectorDeltaPages.push_back(manifest);
			publishedVectorDeltaManifests.push_back(actual.manifest_location);
			erasedVectorDeltaIds.insert(erasedVectorDeltaIds.end(), mutation.erased_vector_ids.begin(),
										mutation.erased_vector_ids.end());
		}

		std::vector<PageLocation> publishedPages;
		size_t propertyPageCount = 0U;
		for (const NativeGraphPropertyPageBatchMaterialization &materialization:
			 propertyPageBatchMaterializations) {
			propertyPageCount += materialization.pages.size();
		}
		publishedPages.reserve(materializations.size() + appendMaterializations.size() +
							   newPoolMaterializations.size() + propertyMaterializations.size() +
							   propertyPageCount + publishedLargeValuePages.size() +
							   publishedDerivedIndexPages.size() + publishedVectorBootstrapPages.size() +
							   publishedVectorDeltaPages.size());
		for (const EntityPageReplacementMaterialization &materialization: materializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const EntityPageAppendBatchMaterialization &materialization: appendMaterializations) {
			publishedPages.push_back(materialization.appended_location);
		}
		for (const EntityPageNewPoolBatchMaterialization &materialization: newPoolMaterializations) {
			publishedPages.push_back(materialization.initial_location);
		}
		for (const NativeGraphPropertyPageReplacementMaterialization &materialization: propertyMaterializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				publishedPages.push_back(page.location);
			}
		}
		publishedPages.insert(publishedPages.end(), publishedLargeValuePages.begin(), publishedLargeValuePages.end());
		publishedPages.insert(publishedPages.end(), publishedDerivedIndexPages.begin(),
							  publishedDerivedIndexPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorBootstrapPages.begin(),
							  publishedVectorBootstrapPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorDeltaPages.begin(), publishedVectorDeltaPages.end());
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		std::ranges::sort(retiredPageIds);
		const bool duplicatePublishedPage =
				std::ranges::adjacent_find(publishedPages, {}, &PageLocation::page_id) != publishedPages.end();
		const bool duplicateRetiredPage = std::ranges::adjacent_find(retiredPageIds) != retiredPageIds.end();
		const bool publishedAndRetired =
				std::ranges::any_of(publishedPages, [&retiredPageIds](const PageLocation &page) {
					return std::ranges::binary_search(retiredPageIds, page.page_id.value());
				});
		if (duplicatePublishedPage || duplicateRetiredPage || publishedAndRetired) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph publication has duplicate or contradictory Page Directory mutations");
		}
		// A transaction may publish only a durable logical-ID reservation after a
		// provisional graph result is cancelled. That advances the Superblock
		// high-water marks, but it must not manufacture an empty Page Directory
		// mutation merely to satisfy the generic graph publication path.
		if (!publishedPages.empty() || !retiredPageIds.empty()) {
			if (retiredPageIds.empty()) {
				locations_.prepareUpsertPages(batch, publishedPages);
			} else {
				locations_.prepareReplacePages(batch, publishedPages, retiredPageIds);
			}
		}
		if (!insertedNodes.empty() || !erasedNodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, insertedNodes, erasedNodes);
		}
		if (!insertedEdges.empty() || !erasedEdges.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, insertedEdges, erasedEdges);
		}
		std::vector<EntityPropertyKey> erasedProperties;
		erasedProperties.reserve(explicitlyErasedEntityProperties.size() + erasedEntityProperties.size());
		erasedProperties.insert(erasedProperties.end(), explicitlyErasedEntityProperties.begin(),
								explicitlyErasedEntityProperties.end());
		erasedProperties.insert(erasedProperties.end(), erasedEntityProperties.begin(), erasedEntityProperties.end());
		std::ranges::sort(upsertedEntityProperties, {},
						  [](const EntityPropertyLocation &location) { return location.key.directoryId(); });
		std::ranges::sort(erasedProperties, {}, &EntityPropertyKey::directoryId);
		if (!upsertedEntityProperties.empty() || !erasedProperties.empty()) {
			locations_.prepareReplaceEntityProperties(batch, upsertedEntityProperties, erasedProperties);
		}
		if (!publishedDerivedIndexRoots.empty() || !erasedDerivedIndexRootIds.empty()) {
			locations_.prepareReplaceIndexRoots(batch, publishedDerivedIndexRoots, erasedDerivedIndexRootIds);
		}
		std::vector<VectorLocation> publishedVectorLocations;
		publishedVectorLocations.reserve(publishedVectorBootstrapLocations.size() +
										 publishedVectorDeltaLocations.size());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorBootstrapLocations.begin(),
										publishedVectorBootstrapLocations.end());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorDeltaLocations.begin(),
										publishedVectorDeltaLocations.end());
		std::ranges::sort(publishedVectorLocations, {}, &VectorLocation::vector_id);
		std::ranges::sort(erasedVectorDeltaIds);
		if (std::ranges::adjacent_find(publishedVectorLocations, {}, &VectorLocation::vector_id) !=
					publishedVectorLocations.end() ||
			std::ranges::adjacent_find(erasedVectorDeltaIds) != erasedVectorDeltaIds.end() ||
			std::ranges::any_of(publishedVectorLocations, [&erasedVectorDeltaIds](const VectorLocation &location) {
				return std::ranges::binary_search(erasedVectorDeltaIds, location.vector_id);
			})) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"native graph Vector publication has duplicate or contradictory Vector Location mutations");
		}
		if (!publishedVectorLocations.empty() || !erasedVectorDeltaIds.empty()) {
			locations_.prepareReplaceVectorLocations(batch, publishedVectorLocations, erasedVectorDeltaIds);
		}
		std::vector<VectorManifestLocation> publishedVectorManifests;
		publishedVectorManifests.reserve(publishedVectorBootstrapManifests.size() +
										 publishedVectorDeltaManifests.size());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorBootstrapManifests.begin(),
										publishedVectorBootstrapManifests.end());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorDeltaManifests.begin(),
										publishedVectorDeltaManifests.end());
		std::ranges::sort(publishedVectorManifests, {}, &VectorManifestLocation::index_id);
		if (std::ranges::adjacent_find(publishedVectorManifests, {}, &VectorManifestLocation::index_id) !=
			publishedVectorManifests.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph Vector publication has duplicate Manifest mutations");
		}
		if (!publishedVectorManifests.empty()) {
			locations_.prepareReplaceVectorManifests(batch, publishedVectorManifests, {});
		}
		if (!publishedLargeValueManifests.empty() || !erasedLargeValueIds.empty()) {
			locations_.prepareReplaceLargeValueManifests(batch, publishedLargeValueManifests, erasedLargeValueIds);
		}
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
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
					if (!newPools.empty()) {
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
					if (!appends.empty()) {
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retirePagesInExistingPools(
			Generation expectedMetadataGeneration, std::span<const IndexRootLocation> erasedRoots,
			std::span<const PageRetirement> retirements, LogSequenceNumber checkpointLsn,
			const ExistingPoolPagesRetirementPreparer &preparer, const MetadataPublishOptions &options,
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
			const bool largeValueManifest =
					retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(visibleLocation != retirement.location) |
					static_cast<uint8_t>(pool.segment_id != retirement.location.segment_id) |
					static_cast<uint8_t>(pool.generation != retirement.location.generation) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isSharedPagePoolSegmentKind(pool.kind)) |
					static_cast<uint8_t>(!matchesSharedPagePoolOwnerKind(pool.kind, retirement.expected_owner_kind)) |
					static_cast<uint8_t>(!supportsPageDirectoryOnlyRetirement(retirement.expected_owner_kind)) |
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return AllocatorReservation{batch.candidate(), *physicalSlot};
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot,
													checkpointLsn, nullptr, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable prepared Catalog reservation requires a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot,
													checkpointLsn, &preparer, nullptr, {}, options);
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
			PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer *preparer,
			const EntityPagePoolReservationPreparer *entityPagePreparer, std::span<PageLocation> mutableEntityPages,
			const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const CatalogReservationPublicationRequest request{
				requests,	 pages,		extents,  entityPageNodes, entityPageEdges,	   manifests,
				firstSlot, lastSlot, preparer,		   entityPagePreparer, mutableEntityPages};
		std::optional<std::vector<CatalogEntry>> entries = reservation_coordinator_.prepare(batch, request);
		if (!entries.has_value()) {
			return std::nullopt;
		}
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_retirement_coordinator_.prepareRetirement(batch, segmentId, expectedGeneration);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::releaseRetiredCatalogSegment(SegmentId segmentId,
																	  Generation expectedGeneration,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const RetireQueueEntry retirement =
				segment_retirement_coordinator_.validateRetiredRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable retired slot release is blocked by a snapshot that can resolve the retired slot");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_retirement_coordinator_.prepareRetiredRelease(batch, retirement);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return CatalogSlotRelocation{batch.candidate(), plan->source, plan->destination};
	}

	Superblock RelocatableMetadataStore::releaseRelocatedCatalogSource(SegmentId segmentId,
																	   Generation expectedGeneration,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const RetireQueueEntry retirement =
				segment_relocation_coordinator_.validateRelocatedSourceRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable relocated source release is blocked by a snapshot that can resolve the source");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_relocation_coordinator_.prepareRelocatedSourceRelease(batch, retirement);
		pool_ownership_.preparePublication(batch);
		batch.publish(bootstrap_, options.bootstrap_failpoint);
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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

		std::vector<CatalogEntry> candidates;
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
		if (candidates.empty()) {
			return MetadataPagePoolReclamation{current, 0U, {}};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<PhysicalSlotId> candidateSlots;
		std::vector<SegmentId> candidateSegmentIds;
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
		catalog_.prepareErase(batch, candidateSegmentIds);
		pool_ownership_.preparePublication(batch);

		try {
			batch.publish(bootstrap_, options.bootstrap_failpoint);
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
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
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
		// Moving a source frame before every local reader has left its old root
		// would invalidate that reader. The writer guard keeps this count stable.
		if (snapshots_.activeSnapshotCount() != 0U) {
			return MetadataPagePoolEvacuation{bootstrap_.recover(), {}, 0U};
		}
		const Superblock current = bootstrap_.recover();
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);
		if (!current.catalog_root.isValid()) {
			// The bootstrap-only new-file state has no durable pool ownership entry.
			// The first normal publication establishes it before evacuation is allowed.
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}

		std::optional<CatalogEntry> source;
		uint32_t sourceLiveFrames = 0U;
		catalog_.visit(current, [this, &current, maxLiveFrames, &source, &sourceLiveFrames](const CatalogEntry &entry) {
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
			if (liveFrames == 0U || liveFrames > maxLiveFrames ||
				(source.has_value() &&
				 (liveFrames > sourceLiveFrames ||
				  (liveFrames == sourceLiveFrames && entry.physical_slot >= source->physical_slot)))) {
				return;
			}
			source = entry;
			sourceLiveFrames = liveFrames;
		});
		if (!source.has_value()) {
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		// Exclude the source before the first COW allocation. In particular the
		// Catalog or Allocator root rewrite must never refill the pool it evicts.
		batch.reserveDataSlot(source->physical_slot);
		cloneMetadataDirectoryRootsForEvacuation(pages_, batch, current);
		pool_ownership_.preparePublication(batch);
		try {
			batch.publish(bootstrap_, options.bootstrap_failpoint);
		} catch (const BootstrapPublishInterrupted &) {
			if (bootstrap_.recover() == batch.candidate()) {
				snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
			}
			throw;
		}
		// Only the durable new root makes the source frames unreachable. This
		// reconciliation is the publication-safe transition to an empty pool;
		// the next reclaimer transaction removes its Catalog ownership and bit.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		if (!metadata_page_pool_.isMetadataPoolEmpty(source->physical_slot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool evacuation left reachable frames in its source pool");
		}
		return MetadataPagePoolEvacuation{batch.candidate(), source->physical_slot, sourceLiveFrames};
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
			successor = metadata_page_pool_.rotateEmergencyReserve(successorToken, batch.generation(), checkpointLsn);
			batch.trackUnpublishedPool(successor.physical_slot);
			batch.setMetadataEmergencyReserve(successor);
			// The successor has no Catalog entry by design, but its physical slot is
			// already part of the candidate allocator root before publication.
			allocator_.prepareSetAllocated(batch, successor.physical_slot, true);
			// The former reserve retains its permanent token and is now added as a
			// normal Catalog-owned pool in this same COW generation.
			pool_ownership_.preparePublication(batch);
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
		return catalog_.find(snapshot.superblock(), segmentId);
	}

	std::optional<RetireQueueEntry>
	RelocatableMetadataStore::findRetireQueueEntryOptional(const MetadataSnapshotPin &snapshot,
														   PhysicalSlotId slot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue lookup requires this store's active snapshot pin");
		}
		return locations_.findRetireQueueEntryOptional(snapshot.superblock(), slot);
	}

	void RelocatableMetadataStore::visitRetireQueueEntries(
			const MetadataSnapshotPin &snapshot, const std::function<void(const RetireQueueEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Retire Queue traversal requires this store's active snapshot and a visitor");
		}
		locations_.visitRetireQueue(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitCatalogEntries(const MetadataSnapshotPin &snapshot,
													   const std::function<void(const CatalogEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog traversal requires this store's active snapshot and a visitor");
		}
		catalog_.visit(snapshot.superblock(), visitor);
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentIndexPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable index page-pool scan requires this store's active snapshot pin");
		}
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
		return locations_.findPage(snapshot.superblock(), pageId);
	}

	std::optional<PageLocation> RelocatableMetadataStore::findPageLocationOptional(const MetadataSnapshotPin &snapshot,
																				   PageId pageId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Page Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findPageOptional(snapshot.superblock(), pageId);
	}

	ExtentLocation RelocatableMetadataStore::findExtentLocation(const MetadataSnapshotPin &snapshot,
																ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Extent Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findExtent(snapshot.superblock(), extentId);
	}

	std::optional<ExtentLocation>
	RelocatableMetadataStore::findExtentLocationOptional(const MetadataSnapshotPin &snapshot, ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Extent Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findExtentOptional(snapshot.superblock(), extentId);
	}

	EntityPageLocation RelocatableMetadataStore::findNodePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Node lookup requires this store's active snapshot pin");
		}
		return locations_.findNodePageLocation(snapshot.superblock(), nodeId);
	}

	EntityPageLocation RelocatableMetadataStore::findEdgePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Edge lookup requires this store's active snapshot pin");
		}
		return locations_.findEdgePageLocation(snapshot.superblock(), edgeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findNodePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Node lookup requires this store's active snapshot pin");
		}
		return locations_.findNodePageLocationOptional(snapshot.superblock(), nodeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findEdgePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Edge lookup requires this store's active snapshot pin");
		}
		return locations_.findEdgePageLocationOptional(snapshot.superblock(), edgeId);
	}

	void RelocatableMetadataStore::visitNodePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		locations_.visitNodePageLocations(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitEdgePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
		locations_.visitEdgePageLocations(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitNodePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		return locations_.visitNodePageLocationsUntil(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitEdgePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
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
		locations_.visitEntityProperties(snapshot.superblock(), visitor);
	}

	void
	RelocatableMetadataStore::visitStateLocations(const MetadataSnapshotPin &snapshot,
												  const std::function<void(const StateLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State directory traversal requires this store's active snapshot and a visitor");
		}
		locations_.visitStates(snapshot.superblock(), visitor);
	}

	void
	RelocatableMetadataStore::visitIndexRoots(const MetadataSnapshotPin &snapshot,
											  const std::function<void(const IndexRootLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Index Root directory traversal requires this store's active snapshot and a visitor");
		}
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
		locations_.visitLargeValueManifests(snapshot.superblock(), visitor);
	}

	EntityPropertyLocation RelocatableMetadataStore::findEntityPropertyLocation(const MetadataSnapshotPin &snapshot,
																				const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Property Directory lookup requires a valid key and snapshot");
		}
		return locations_.findEntityProperty(snapshot.superblock(), key);
	}

	std::optional<EntityPropertyLocation>
	RelocatableMetadataStore::findEntityPropertyLocationOptional(const MetadataSnapshotPin &snapshot,
																 const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable optional Entity Property lookup requires a valid key and snapshot");
		}
		return locations_.findEntityPropertyOptional(snapshot.superblock(), key);
	}

	StateLocation RelocatableMetadataStore::findStateLocation(const MetadataSnapshotPin &snapshot,
															  uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findState(snapshot.superblock(), stateId);
	}

	std::optional<StateLocation>
	RelocatableMetadataStore::findStateLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional State Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findStateOptional(snapshot.superblock(), stateId);
	}

	VectorLocation RelocatableMetadataStore::findVectorLocation(const MetadataSnapshotPin &snapshot,
																uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Location Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findVectorLocation(snapshot.superblock(), vectorId);
	}

	std::optional<VectorLocation>
	RelocatableMetadataStore::findVectorLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Location Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findVectorLocationOptional(snapshot.superblock(), vectorId);
	}

	VectorManifestLocation RelocatableMetadataStore::findVectorManifest(const MetadataSnapshotPin &snapshot,
																		uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findVectorManifest(snapshot.superblock(), indexId);
	}

	std::optional<VectorManifestLocation>
	RelocatableMetadataStore::findVectorManifestOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findVectorManifestOptional(snapshot.superblock(), indexId);
	}

	LargeValueManifestLocation RelocatableMetadataStore::findLargeValueManifest(const MetadataSnapshotPin &snapshot,
																				LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable LargeValue Manifest Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findLargeValueManifest(snapshot.superblock(), valueId);
	}

	std::optional<LargeValueManifestLocation>
	RelocatableMetadataStore::findLargeValueManifestOptional(const MetadataSnapshotPin &snapshot,
															 LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable optional LargeValue Manifest Directory "
																   "lookup requires this store's active snapshot pin");
		}
		return locations_.findLargeValueManifestOptional(snapshot.superblock(), valueId);
	}

	IndexRootLocation RelocatableMetadataStore::findIndexRoot(const MetadataSnapshotPin &snapshot,
															  uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Index Root Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findIndexRoot(snapshot.superblock(), indexId);
	}

	std::optional<IndexRootLocation>
	RelocatableMetadataStore::findIndexRootOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Index Root Directory lookup requires this store's active snapshot pin");
		}
		return locations_.findIndexRootOptional(snapshot.superblock(), indexId);
	}

	std::vector<PageLocation> RelocatableMetadataStore::findPageLocationsForSegment(const MetadataSnapshotPin &snapshot,
																					SegmentId segmentId) const {
		if (!snapshots_.owns(snapshot) || !segmentId.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Page directory scan requires this store's active snapshot and a Segment ID");
		}
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
		io_->flushStream();
		io_->sync();
		if (portable_ftruncate(nativeHandle, retainedBytes) != 0) {
			return {RelocatableTailTruncationStatus::VTTS_IO_ERROR, originalBytes, originalBytes, 0};
		}
		io_->sync();
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

	uint64_t RelocatableMetadataStore::activeSnapshotCount() const { return snapshots_.activeSnapshotCount(); }


} // namespace graph::storage
