/**
 * @file CatalogRetireQueueReclaimer.cpp
 * @brief Snapshot-safe batch reclamation for Catalog-backed physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogRetireQueueReclaimer.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	CatalogRetireQueueReclaimer::CatalogRetireQueueReclaimer(CatalogTree &catalog, AllocatorDirectory &allocator,
															 LocationDirectory &locations,
															 MetadataRetirement &retirement) :
		catalog_(catalog), allocator_(allocator), locations_(locations), retirement_(retirement) {}

	void CatalogRetireQueueReclaimer::validateEntry(const Superblock &current, const RetireQueueEntry &entry) const {
		const std::optional<CatalogEntry> currentEntry = catalog_.findOptional(current, entry.segment_id);
		const uint8_t invalidCommon = static_cast<uint8_t>(!entry.physical_slot.isValid()) |
									  static_cast<uint8_t>(!entry.segment_id.isValid()) |
									  static_cast<uint8_t>(!entry.retired_generation.isValid()) |
									  static_cast<uint8_t>(entry.retired_generation > current.generation) |
									  static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot)) |
									  static_cast<uint8_t>(!currentEntry.has_value());
		if (invalidCommon != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue has an invalid or unowned physical slot");
		}

		switch (entry.disposition) {
			case RetireQueueDisposition::RQD_RELOCATED_SOURCE:
				if (currentEntry->physical_slot == entry.physical_slot ||
					currentEntry->state == SegmentState::SGS_OBSOLETE) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Retire Queue relocated source still has a current Catalog mapping");
				}
				return;
			case RetireQueueDisposition::RQD_RETIRED_SEGMENT:
				if (currentEntry->state != SegmentState::SGS_RETIRED ||
					currentEntry->physical_slot != entry.physical_slot) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable Retire Queue retired Segment disagrees with Catalog state");
				}
				return;
			default:
				throw FormatException(FormatError::FERR_INVALID_ENUM,
									  "relocatable Retire Queue has an unknown disposition");
		}
	}

	std::vector<RetireQueueEntry>
	CatalogRetireQueueReclaimer::collectReclaimable(const Superblock &current, const MetadataWriteGuard &writer) const {
		std::vector<RetireQueueEntry> reclaimable;
		std::map<SegmentId, PhysicalSlotId> seenSegments;
		locations_.visitRetireQueue(current, [this, &current, &writer, &reclaimable,
											  &seenSegments](const RetireQueueEntry &entry) {
			validateEntry(current, entry);
			if (!seenSegments.emplace(entry.segment_id, entry.physical_slot).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Retire Queue has multiple entries for one logical Segment ID");
			}
			if (!retirement_.isVisibleToActiveSnapshot(writer, entry)) {
				reclaimable.push_back(entry);
			}
		});
		return reclaimable;
	}

	void CatalogRetireQueueReclaimer::prepareReclamation(MetadataWriteBatch &batch,
														 std::span<const RetireQueueEntry> entries) const {
		if (entries.empty()) {
			return;
		}

		const Superblock &current = batch.current();
		std::map<SegmentId, CatalogEntry> catalogUpdates;
		PhysicalSlotId previousSlot;
		for (const RetireQueueEntry &entry: entries) {
			const std::optional<RetireQueueEntry> durableEntry =
					locations_.findRetireQueueEntryOptional(current, entry.physical_slot);
			if ((previousSlot.isValid() && entry.physical_slot <= previousSlot) || !durableEntry.has_value() ||
				*durableEntry != entry) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Retire Queue reclamation entries do not match the current durable queue");
			}
			validateEntry(current, entry);
			const CatalogEntry currentEntry = catalog_.find(current, entry.segment_id);
			if (!catalogUpdates.emplace(entry.segment_id, currentEntry).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Retire Queue reclamation has duplicate logical Segment IDs");
			}
			if (entry.disposition == RetireQueueDisposition::RQD_RETIRED_SEGMENT) {
				catalogUpdates.at(entry.segment_id).state = SegmentState::SGS_OBSOLETE;
			}
			previousSlot = entry.physical_slot;
		}

		std::vector<PhysicalSlotId> erasedSlots;
		erasedSlots.reserve(entries.size());
		for (const RetireQueueEntry &entry: entries) {
			// Keep every source unavailable until one COW generation both erases its
			// queue entry and clears its allocator bit.
			batch.reserveDataSlot(entry.physical_slot);
			erasedSlots.push_back(entry.physical_slot);
		}
		std::vector<CatalogEntry> updates;
		updates.reserve(catalogUpdates.size());
		for (const auto &[segmentId, entry]: catalogUpdates) {
			static_cast<void>(segmentId);
			updates.push_back(entry);
		}
		catalog_.prepareUpsert(batch, updates);
		const std::array<RetireQueueEntry, 0> noInsertions{};
		locations_.prepareReplaceRetireQueue(batch, noInsertions, erasedSlots);
		for (const RetireQueueEntry &entry: entries) {
			allocator_.prepareSetAllocated(batch, entry.physical_slot, false);
		}
	}

} // namespace graph::storage
