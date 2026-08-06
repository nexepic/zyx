/**
 * @file CatalogSegmentRetirementCoordinator.cpp
 * @brief Pre-publication lifecycle transitions for Catalog-backed segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogSegmentRetirementCoordinator.hpp"

#include <array>
#include <cstdint>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	CatalogSegmentRetirementCoordinator::CatalogSegmentRetirementCoordinator(
			CatalogTree &catalog, AllocatorDirectory &allocator, LocationDirectory &locations, MetadataRetirement &retirement) :
		catalog_(catalog), allocator_(allocator), locations_(locations), retirement_(retirement) {}

	void CatalogSegmentRetirementCoordinator::prepareRetirement(MetadataWriteBatch &batch, SegmentId segmentId,
																			 Generation expectedGeneration) const {
		const Superblock &current = batch.current();
		const CatalogEntry currentEntry = catalog_.find(current, segmentId);
		const uint8_t invalid = static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(currentEntry.generation != expectedGeneration) |
				static_cast<uint8_t>(currentEntry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, currentEntry.physical_slot));
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog retirement requires one allocated sealed non-Catalog generation");
		}
		if (isSharedPagePoolSegmentKind(currentEntry.kind)) {
			bool hasCurrentPage = false;
			locations_.visitPages(current, [&currentEntry, &hasCurrentPage](const PageLocation &location) {
				hasCurrentPage = static_cast<bool>(static_cast<uint8_t>(hasCurrentPage) |
													 static_cast<uint8_t>(location.segment_id == currentEntry.segment_id));
			});
			if (hasCurrentPage) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable shared page pool retirement requires an empty current Page Directory");
			}
		}

		CatalogEntry retired = currentEntry;
		retired.state = SegmentState::SGS_RETIRED;
		batch.reserveDataSlot(retired.physical_slot);
		catalog_.prepareUpsert(batch, std::span(&retired, 1U));
		const RetireQueueEntry retirement{retired.physical_slot, retired.segment_id, batch.generation(),
															RetireQueueDisposition::RQD_RETIRED_SEGMENT};
		retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1U));
	}

	RetireQueueEntry CatalogSegmentRetirementCoordinator::validateRetiredRelease(const Superblock &current,
			SegmentId segmentId, Generation expectedGeneration) const {
		const CatalogEntry currentEntry = catalog_.find(current, segmentId);
		const std::optional<RetireQueueEntry> queued =
				locations_.findRetireQueueEntryOptional(current, currentEntry.physical_slot);
		if (!queued.has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable retired slot release requires one allocated retired non-Catalog generation");
		}
		const uint8_t invalid = static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_RETIRED) |
				static_cast<uint8_t>(currentEntry.generation != expectedGeneration) |
				static_cast<uint8_t>(currentEntry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, currentEntry.physical_slot)) |
				static_cast<uint8_t>(queued->segment_id != segmentId) |
				static_cast<uint8_t>(queued->disposition != RetireQueueDisposition::RQD_RETIRED_SEGMENT);
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable retired slot release requires one allocated retired non-Catalog generation");
		}
		return *queued;
	}

	void CatalogSegmentRetirementCoordinator::prepareRetiredRelease(MetadataWriteBatch &batch,
			const RetireQueueEntry &retirement) const {
		const CatalogEntry currentEntry = catalog_.find(batch.current(), retirement.segment_id);
		const uint8_t invalid = static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_RETIRED) |
				static_cast<uint8_t>(currentEntry.physical_slot != retirement.physical_slot);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable retired slot release no longer matches the current Catalog mapping");
		}
		CatalogEntry released = currentEntry;
		released.state = SegmentState::SGS_OBSOLETE;
		batch.reserveDataSlot(released.physical_slot);
		catalog_.prepareUpsert(batch, std::span(&released, 1U));
		const std::array<RetireQueueEntry, 0> noInsertions{};
		const std::array erasedSlots{retirement.physical_slot};
		locations_.prepareReplaceRetireQueue(batch, noInsertions, erasedSlots);
		allocator_.prepareSetAllocated(batch, released.physical_slot, false);
	}

} // namespace graph::storage
