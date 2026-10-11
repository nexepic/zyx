/**
 * @file CatalogSegmentRelocationCoordinator.cpp
 * @brief Pre-publication relocation coordination for Catalog-backed segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogSegmentRelocationCoordinator.hpp"

#include <array>
#include <cstdint>
#include <limits>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	CatalogSegmentRelocationCoordinator::CatalogSegmentRelocationCoordinator(CatalogTree &catalog,
																			 AllocatorDirectory &allocator,
																			 LocationDirectory &locations,
																			 MetadataRetirement &retirement) :
		catalog_(catalog), allocator_(allocator), locations_(locations), retirement_(retirement) {}

	std::optional<RetireQueueEntry> CatalogSegmentRelocationCoordinator::findPendingSource(const Superblock &current,
																						   SegmentId segmentId) const {
		std::optional<RetireQueueEntry> result;
		locations_.visitRetireQueue(current, [&result, segmentId](const RetireQueueEntry &entry) {
			if (entry.segment_id != segmentId || entry.disposition != RetireQueueDisposition::RQD_RELOCATED_SOURCE) {
				return;
			}
			if (result.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog relocation has multiple queued sources for one Segment ID");
			}
			result = entry;
		});
		return result;
	}

	std::optional<CatalogSlotRelocationPlan> CatalogSegmentRelocationCoordinator::prepareSegmentRelocation(
			MetadataWriteBatch &batch, SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot,
			PhysicalSlotId lastSlot, const CatalogSlotRelocationPreparer &preparer) const {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog relocation requires a data-copy preparation callback");
		}
		const Superblock &current = batch.current();
		const CatalogEntry source = catalog_.find(current, segmentId);
		const uint8_t invalid =
				static_cast<uint8_t>(source.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(source.generation != expectedGeneration) |
				static_cast<uint8_t>(source.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(source.generation.value() == std::numeric_limits<uint64_t>::max()) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, source.physical_slot));
		if (invalid != 0U || findPendingSource(current, segmentId).has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Catalog relocation requires one allocated sealed generation without a pending source");
		}

		const std::optional<PhysicalSlotId> destinationSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!destinationSlot.has_value()) {
			return std::nullopt;
		}
		CatalogEntry destination = source;
		destination.physical_slot = *destinationSlot;
		destination.generation = Generation(source.generation.value() + 1U);
		destination.committed_lsn = batch.checkpointLsn();
		catalog_.prepareUpsert(batch, std::span(&destination, 1U));
		const RetireQueueEntry retirement{source.physical_slot, source.segment_id, batch.generation(),
										  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
		retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1U));
		preparer(source, destination);
		return CatalogSlotRelocationPlan{source, destination};
	}

	std::optional<CatalogSlotRelocationPlan> CatalogSegmentRelocationCoordinator::prepareExtentRelocation(
			MetadataWriteBatch &batch, const ExtentLocation &expectedExtent, PhysicalSlotId firstSlot,
			PhysicalSlotId lastSlot, const CatalogSlotRelocationPreparer &preparer) const {
		const uint8_t invalidRequest =
				static_cast<uint8_t>(!preparer) | static_cast<uint8_t>(!expectedExtent.extent_id.isValid()) |
				static_cast<uint8_t>(!expectedExtent.segment_id.isValid()) |
				static_cast<uint8_t>(!expectedExtent.generation.isValid()) |
				static_cast<uint8_t>(expectedExtent.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedExtent.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidRequest != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable extent relocation requires one full immutable extent and data preparer");
		}

		const Superblock &current = batch.current();
		const CatalogEntry source = catalog_.find(current, expectedExtent.segment_id);
		const std::optional<ExtentLocation> visibleExtent =
				locations_.findExtentOptional(current, expectedExtent.extent_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(source.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(source.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
				static_cast<uint8_t>(source.generation != expectedExtent.generation) |
				static_cast<uint8_t>(source.generation.value() == std::numeric_limits<uint64_t>::max()) |
				static_cast<uint8_t>(!visibleExtent.has_value()) |
				static_cast<uint8_t>(visibleExtent.has_value() && *visibleExtent != expectedExtent) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, source.physical_slot));
		if (invalidSource != 0U || findPendingSource(current, expectedExtent.segment_id).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable extent relocation requires one current allocated full extent mapping");
		}

		const std::optional<PhysicalSlotId> destinationSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!destinationSlot.has_value()) {
			return std::nullopt;
		}
		CatalogEntry destination = source;
		destination.physical_slot = *destinationSlot;
		destination.generation = Generation(source.generation.value() + 1U);
		destination.committed_lsn = batch.checkpointLsn();
		const ExtentLocation replacement{expectedExtent.extent_id, source.segment_id, expectedExtent.first_page,
										 expectedExtent.page_count, destination.generation};
		catalog_.prepareUpsert(batch, std::span(&destination, 1U));
		const RetireQueueEntry retirement{source.physical_slot, source.segment_id, batch.generation(),
										  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
		retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1U));
		locations_.prepareUpsertExtents(batch, std::span(&replacement, 1U));
		preparer(source, destination);
		return CatalogSlotRelocationPlan{source, destination};
	}

	RetireQueueEntry
	CatalogSegmentRelocationCoordinator::validateRelocatedSourceRelease(const Superblock &current, SegmentId segmentId,
																		Generation expectedGeneration) const {
		const CatalogEntry currentEntry = catalog_.find(current, segmentId);
		const std::optional<RetireQueueEntry> queued = findPendingSource(current, segmentId);
		const uint8_t invalidIdentity =
				static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(currentEntry.generation != expectedGeneration) |
				static_cast<uint8_t>(currentEntry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(!queued.has_value()) |
				static_cast<uint8_t>(queued.has_value() && queued->retired_generation > current.generation) |
				static_cast<uint8_t>(queued.has_value() && queued->retired_generation < currentEntry.generation) |
				static_cast<uint8_t>(queued.has_value() && currentEntry.physical_slot == queued->physical_slot);
		if (invalidIdentity != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable relocated source release requires one allocated relocation generation");
		}
		const uint8_t invalidAllocation =
				static_cast<uint8_t>(!allocator_.isAllocated(current, currentEntry.physical_slot)) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, queued->physical_slot));
		if (invalidAllocation != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable relocated source release requires both physical slots to remain allocated");
		}
		return *queued;
	}

	void CatalogSegmentRelocationCoordinator::prepareRelocatedSourceRelease(MetadataWriteBatch &batch,
																			const RetireQueueEntry &retirement) const {
		const Superblock &current = batch.current();
		const CatalogEntry currentEntry = catalog_.find(current, retirement.segment_id);
		const std::optional<RetireQueueEntry> queued =
				locations_.findRetireQueueEntryOptional(current, retirement.physical_slot);
		const uint8_t invalid =
				static_cast<uint8_t>(retirement.disposition != RetireQueueDisposition::RQD_RELOCATED_SOURCE) |
				static_cast<uint8_t>(!queued.has_value()) |
				static_cast<uint8_t>(queued.has_value() && *queued != retirement) |
				static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(currentEntry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(retirement.retired_generation > current.generation) |
				static_cast<uint8_t>(retirement.retired_generation < currentEntry.generation) |
				static_cast<uint8_t>(currentEntry.physical_slot == retirement.physical_slot) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, currentEntry.physical_slot)) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, retirement.physical_slot));
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable relocated source release no longer matches the current Catalog mapping");
		}
		batch.reserveDataSlot(retirement.physical_slot);
		const std::array<RetireQueueEntry, 0> noInsertions{};
		const std::array erasedSlots{retirement.physical_slot};
		locations_.prepareReplaceRetireQueue(batch, noInsertions, erasedSlots);
		allocator_.prepareSetAllocated(batch, retirement.physical_slot, false);
	}

} // namespace graph::storage
