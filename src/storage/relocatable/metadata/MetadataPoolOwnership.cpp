/**
 * @file MetadataPoolOwnership.cpp
 * @brief Catalog/allocator reconciliation for metadata page-pool ownership.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPoolOwnership.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/utils/OwnedAssociativeContainers.hpp"
#include "graph/utils/OwnedStdVector.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] constexpr uint8_t ownershipFlag(bool condition) { return static_cast<uint8_t>(condition); }

        // Published ownership is established during recovery and preserved by
        // typed mutations. Revalidate the pools touched by this candidate, plus
        // reserve transitions; unrelated ownership is checked by full integrity.
        [[nodiscard]] utils::OwnedSet<uint64_t> publicationPoolSlots(
            MetadataPagePoolAllocator &pools, const MetadataWriteBatch &batch) {
            utils::OwnedSet<uint64_t> slots;
            if (!batch.current().catalog_root.isValid()) {
                debug::PerfTrace::addValue("native_metadata.ownership.initial_pool_enumerations", 1);
                for (const auto slot : pools.poolSlots()) slots.emplace(slot.value());
                return slots;
            }
            for (const auto slot : pools.unconfirmedOwnershipSlots()) slots.emplace(slot.value());
            for (const auto reference : batch.allocatedMetadataPages()) slots.emplace(reference.physical_slot.value());
            for (size_t i = 0U; i < batch.createdPoolCount(); ++i) slots.emplace(batch.createdPoolSlot(i).value());
            if (batch.current().metadata_emergency_reserve.isValid())
                slots.emplace(batch.current().metadata_emergency_reserve.physical_slot.value());
            if (batch.candidate().metadata_emergency_reserve.isValid())
                slots.emplace(batch.candidate().metadata_emergency_reserve.physical_slot.value());
            return slots;
        }


	} // namespace

	MetadataPoolOwnership::MetadataPoolOwnership(CatalogTree &catalog, AllocatorDirectory &allocator,
												 MetadataPagePoolAllocator &metadataPagePool, bool readOnly) :
		catalog_(catalog), allocator_(allocator), metadata_page_pool_(metadataPagePool), read_only_(readOnly) {}

	void MetadataPoolOwnership::restoreCatalogOwnership(const Superblock &current) {
		if (!current.catalog_root.isValid()) {
			// A newly initialized file has an Allocator root before it has a
			// Catalog root. Its bootstrap metadata pool is recorded by the first
			// metadata publication, so there is no full-file discovery here.
			return;
		}

		std::map<uint64_t, CatalogEntry> catalogPools;
		catalog_.visit(current, [this, &catalogPools, &current](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL) {
				return;
			}
			if (entry.state == SegmentState::SGS_OBSOLETE) {
				if (entry.owner_id != entry.physical_slot.value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable obsolete metadata-pool ownership has an invalid identity");
				}
				return;
			}
			const uint8_t invalid = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(entry.owner_id != entry.physical_slot.value()) |
									static_cast<uint8_t>(entry.generation > current.generation) |
									static_cast<uint8_t>(entry.committed_lsn > current.checkpoint_lsn) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot));
			const bool duplicatePoolSlot = !catalogPools.emplace(entry.physical_slot.value(), entry).second;
			if ((invalid != 0U) | duplicatePoolSlot) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog has an invalid or duplicate metadata-pool ownership entry");
			}
			metadata_page_pool_.registerCatalogPool(entry.physical_slot, entry.segment_id, entry.generation,
													entry.committed_lsn);
		});

		for (const PhysicalSlotId slot: metadata_page_pool_.poolSlots()) {
			const MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(slot);
			const bool isEmergencyReserveSlot =
					(ownershipFlag(current.metadata_emergency_reserve.isValid()) &
					 ownershipFlag(slot == current.metadata_emergency_reserve.physical_slot)) != 0U;
			if (isEmergencyReserveSlot) {
				const uint8_t invalidReserve =
						static_cast<uint8_t>(header.segment_id != current.metadata_emergency_reserve.pool_token) |
						static_cast<uint8_t>(header.generation != current.metadata_emergency_reserve.generation) |
						static_cast<uint8_t>(header.committed_lsn != current.metadata_emergency_reserve.committed_lsn);
				if (invalidReserve != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable metadata emergency reserve disagrees with its bootstrap descriptor");
				}
				continue;
			}
			const auto entry = catalogPools.find(slot.value());
			if (entry == catalogPools.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable reachable metadata pool has no matching durable Catalog ownership");
			}
			// `registerCatalogPool()` above validates this exact header identity
			// before retaining it, so recovery has one authoritative pool-header
			// validation path rather than two divergent checks.
		}
	}

	void MetadataPoolOwnership::preparePublication(MetadataWriteBatch &batch) {
		constexpr size_t MAX_OWNERSHIP_PUBLICATION_PASSES = 64U;
		const auto createdInBatch = [&batch](PhysicalSlotId slot) {
			for (size_t index = 0; index < batch.createdPoolCount(); ++index) {
				if (batch.createdPoolSlot(index) == slot) {
					return true;
				}
			}
			return false;
		};
		utils::OwnedSet<uint64_t> identitiesBoundInBatch;
		utils::OwnedSet<uint64_t> catalogEntriesPreparedInBatch;

		for (size_t pass = 0; pass < MAX_OWNERSHIP_PUBLICATION_PASSES; ++pass) {
			utils::OwnedStdVector<CatalogEntry> newEntries;
            for (const uint64_t value : publicationPoolSlots(metadata_page_pool_, batch)) {
                const PhysicalSlotId slot(value);
                debug::PerfTrace::addValue("native_metadata.ownership.visited_pools", 1);
				MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(slot);
				const bool promotedEmergencyReserve =
						(ownershipFlag(batch.current().metadata_emergency_reserve.isValid()) &
						 ownershipFlag(slot == batch.current().metadata_emergency_reserve.physical_slot) &
						 ownershipFlag(batch.candidate().metadata_emergency_reserve.isValid()) &
						 ownershipFlag(slot != batch.candidate().metadata_emergency_reserve.physical_slot) &
						 ownershipFlag(header.segment_id == batch.current().metadata_emergency_reserve.pool_token)) !=
						0U;
				const bool isCandidateEmergencyReserve =
						(ownershipFlag(batch.candidate().metadata_emergency_reserve.isValid()) &
						 ownershipFlag(slot == batch.candidate().metadata_emergency_reserve.physical_slot)) != 0U;
				if (isCandidateEmergencyReserve) {
					const MetadataEmergencyReserveDescriptor &reserve = batch.candidate().metadata_emergency_reserve;
					const uint8_t invalidReserve = static_cast<uint8_t>(header.segment_id != reserve.pool_token) |
												   static_cast<uint8_t>(header.generation != reserve.generation) |
												   static_cast<uint8_t>(header.committed_lsn != reserve.committed_lsn);
					if (invalidReserve != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable metadata emergency reserve disagrees with its bootstrap descriptor");
					}
					continue;
				}
				const bool newlyCreated = createdInBatch(slot);
				if (newlyCreated && header.segment_id == SegmentId(slot.value()) &&
					!batch.catalogPoolIdentityBound(slot) && identitiesBoundInBatch.emplace(slot.value()).second) {
					metadata_page_pool_.bindCatalogPoolIdentity(slot, batch.allocateLogicalSegmentId(),
												header.generation, header.committed_lsn);
					batch.markCatalogPoolIdentityBound(slot);
					header = metadata_page_pool_.poolHeader(slot);
				}
				if (catalogEntriesPreparedInBatch.contains(slot.value()) ||
					batch.catalogPoolOwnershipPrepared(slot)) {
					// The prior pass already staged this entry in the same candidate
					// Catalog root. Rewriting it again would consume fresh COW pages and
					// can recursively create further pools without changing ownership.
					continue;
				}

				// The current root is the only route context guaranteed to be
				// published while this batch is being assembled. New pools cannot have
				// a current-generation entry by construction; released candidates are
				// distinguished below using the candidate allocator bit.
				const std::optional<CatalogEntry> existing = catalog_.findOptional(batch.current(), header.segment_id);
				if (existing.has_value()) {
					const bool releasedByThisCandidate =
							(ownershipFlag(existing->state == SegmentState::SGS_OBSOLETE) &
							 ownershipFlag(existing->kind == SegmentKind::SGK_METADATA_PAGE_POOL) &
							 ownershipFlag(existing->physical_slot == slot) &
							 ownershipFlag(existing->owner_id == slot.value()) &
														 ownershipFlag(!allocator_.isAllocatedForMutation(batch, slot))) != 0U;
					if (releasedByThisCandidate) {
						continue;
					}
					const uint8_t invalidExisting =
							static_cast<uint8_t>(existing->kind != SegmentKind::SGK_METADATA_PAGE_POOL) |
							static_cast<uint8_t>(existing->state != SegmentState::SGS_SEALED) |
							static_cast<uint8_t>(existing->physical_slot != slot) |
							static_cast<uint8_t>(existing->owner_id != slot.value()) |
							static_cast<uint8_t>(existing->generation != header.generation) |
							static_cast<uint8_t>(existing->committed_lsn != header.committed_lsn);
					if (invalidExisting != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable metadata-pool identity collides with a nonmatching Catalog entry");
					}
                    metadata_page_pool_.registerCatalogPool(slot, header.segment_id, header.generation, header.committed_lsn);
                    batch.markCatalogPoolOwnershipPrepared(slot);
					continue;
				}
				if ((ownershipFlag(batch.isDataSlotReserved(slot)) &
						ownershipFlag(!allocator_.isAllocatedForMutation(batch, slot))) != 0U) {
					// The metadata-pool reclaimer removes Catalog ownership and the
					// allocator bit in this same batch. It retains the pool in memory
					// until the A/B publication succeeds, so do not recreate it here.
					continue;
				}
				if ((ownershipFlag(batch.current().catalog_root.isValid()) & ownershipFlag(!newlyCreated) &
						ownershipFlag(!promotedEmergencyReserve)) != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable published metadata pool is missing its durable Catalog ownership");
				}

				if (!allocator_.isAllocatedForMutation(batch, slot)) {
					allocator_.prepareSetAllocated(batch, slot, true);
				}
				newEntries.push_back(CatalogEntry{header.segment_id, slot, SegmentKind::SGK_METADATA_PAGE_POOL,
											  slot.value(), header.generation, SegmentState::SGS_SEALED,
											  header.committed_lsn});
				catalogEntriesPreparedInBatch.emplace(slot.value());
				batch.markCatalogPoolOwnershipPrepared(slot);
			}

			if (newEntries.empty()) {
				return;
			}
			std::ranges::sort(newEntries, {}, &CatalogEntry::segment_id);
			catalog_.prepareUpsert(batch, newEntries);
		}

		throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata-pool ownership publication did not converge");
	}

	void MetadataPoolOwnership::verifyAfterOpen(const Superblock &current) {
		for (const PhysicalSlotId slot: metadata_page_pool_.poolSlots()) {
			if (allocator_.isAllocated(current, slot)) {
				continue;
			}
			if (metadata_page_pool_.isMetadataPoolEmpty(slot)) {
				// A crash before the Superblock switch leaves no reachable page and no
				// ownership bit. Only writable recovery may return this physical slot
				// to free space; a read-only open must never repair on-disk state.
				if (read_only_) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable read-only metadata open found an unowned metadata pool requiring recovery");
				}
				metadata_page_pool_.abandonUnpublishedPool(slot);
				continue;
			}
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata pool has reachable frames without free-space ownership");
		}
	}

} // namespace graph::storage
