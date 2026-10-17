/**
 * @file CatalogSegmentRetirementCoordinator.cpp
 * @brief Pre-publication lifecycle transitions for Catalog-backed segments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogSegmentRetirementCoordinator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <vector>

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
		const std::array requests{CatalogSegmentRetirementRequest{segmentId, expectedGeneration}};
		prepareRetirements(batch, requests);
	}

	void CatalogSegmentRetirementCoordinator::prepareRetirements(
			MetadataWriteBatch &batch, std::span<const CatalogSegmentRetirementRequest> requests) const {
		if (requests.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog retirement batch requires at least one segment");
		}
		const Superblock &current = batch.current();
		std::set<uint64_t> segmentIds;
		std::set<uint64_t> physicalSlots;
		std::vector<CatalogEntry> retiredEntries;
		std::vector<RetireQueueEntry> retirements;
		retiredEntries.reserve(requests.size());
		retirements.reserve(requests.size());
		for (const CatalogSegmentRetirementRequest &request: requests) {
			if (!request.segment_id.isValid() || !request.expected_generation.isValid() ||
				!segmentIds.insert(request.segment_id.value()).second) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog retirement batch has an invalid or duplicate segment");
			}
			const CatalogEntry currentEntry = catalog_.find(current, request.segment_id);
			const uint8_t invalid = static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(currentEntry.generation != request.expected_generation) |
				static_cast<uint8_t>(currentEntry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, currentEntry.physical_slot)) |
				static_cast<uint8_t>(!physicalSlots.insert(currentEntry.physical_slot.value()).second);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog retirement requires allocated unique sealed generations");
			}
			CatalogEntry retired = currentEntry;
			retired.state = SegmentState::SGS_RETIRED;
			batch.reserveDataSlot(retired.physical_slot);
			retiredEntries.push_back(retired);
			retirements.push_back({retired.physical_slot, retired.segment_id, batch.generation(),
								  RetireQueueDisposition::RQD_RETIRED_SEGMENT});
		}

		std::ranges::sort(retiredEntries, {}, &CatalogEntry::segment_id);
		// Every candidate is checked against the same immutable directory. One
		// authenticated traversal proves emptiness for the entire batch instead
		// of scanning every current page again for each retired pool.
		if (std::ranges::any_of(retiredEntries, [](const CatalogEntry &entry) {
			return isSharedPagePoolSegmentKind(entry.kind);
		})) {
			bool hasCurrentPage = false;
			locations_.visitPages(current, [&](const PageLocation &location) {
				const auto candidate = std::lower_bound(retiredEntries.begin(), retiredEntries.end(),
					location.segment_id.value(), [](const CatalogEntry &entry, uint64_t id) {
						return entry.segment_id.value() < id;
					});
				if (candidate != retiredEntries.end() && candidate->segment_id == location.segment_id &&
					isSharedPagePoolSegmentKind(candidate->kind)) hasCurrentPage = true;
			});
			if (hasCurrentPage)
				throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable shared page pool retirement requires an empty current Page Directory");
		}
		std::ranges::sort(retirements, {}, &RetireQueueEntry::physical_slot);
		catalog_.prepareUpsert(batch, retiredEntries);
		retirement_.prepareQueueInsertions(batch, retirements);
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
				static_cast<uint8_t>(queued->retired_generation > current.generation) |
				static_cast<uint8_t>(queued->retired_generation < currentEntry.generation) |
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
		const Superblock &current = batch.current();
		const CatalogEntry currentEntry = catalog_.find(current, retirement.segment_id);
		const uint8_t invalid = static_cast<uint8_t>(currentEntry.state != SegmentState::SGS_RETIRED) |
				static_cast<uint8_t>(currentEntry.physical_slot != retirement.physical_slot) |
				static_cast<uint8_t>(retirement.retired_generation > current.generation) |
				static_cast<uint8_t>(retirement.retired_generation < currentEntry.generation);
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
