/**
 * @file MetadataReservationCoordinator.cpp
 * @brief Atomic pre-publication coordination for Catalog-backed data slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataReservationCoordinator.hpp"

#include <cstdint>
#include <limits>
#include <ranges>
#include <vector>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	MetadataReservationCoordinator::MetadataReservationCoordinator(CatalogTree &catalog, AllocatorDirectory &allocator,
																	 LocationDirectory &locations) :
		catalog_(catalog), allocator_(allocator), locations_(locations) {}

	std::optional<std::vector<CatalogEntry>> MetadataReservationCoordinator::prepare(
			MetadataWriteBatch &batch, const CatalogReservationPublicationRequest &input) const {
		const std::span<const CatalogSlotReservationRequest> requests = input.reservations;
		if (requests.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable multi-segment reservation requires at least one Catalog request");
		}
		if ((input.data_preparer != nullptr && input.entity_page_preparer != nullptr) ||
			(input.entity_page_preparer == nullptr && !input.mutable_entity_pages.empty()) ||
			(input.entity_page_preparer != nullptr && input.mutable_entity_pages.size() != input.pages.size())) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog reservation has an invalid typed-page materialization callback");
		}
		if (!input.entity_page_nodes.empty() && !input.entity_page_edges.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog reservation cannot mix Node and Edge Entity Map locations");
		}

		SegmentId previous;
		for (const CatalogSlotReservationRequest &request: requests) {
			if (!request.segment_id.isValid() || request.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID ||
				request.kind == SegmentKind::SGK_METADATA_PAGE_POOL ||
				(previous.isValid() && request.segment_id <= previous)) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable multi-segment reservation requests must use allocatable, strictly "
									  "increasing Segment IDs");
			}
			previous = request.segment_id;
		}

		const auto findRequestedLocation = [&requests](SegmentId segmentId, Generation generation) {
			return std::ranges::find_if(requests, [segmentId, generation](const CatalogSlotReservationRequest &request) {
				return request.segment_id == segmentId && request.segment_generation == generation;
			});
		};
		const auto validateRequestedLocation = [&findRequestedLocation, &requests](SegmentId segmentId,
																					 Generation generation) {
			return findRequestedLocation(segmentId, generation) != requests.end();
		};
		for (const PageLocation &location: input.pages) {
			if (!validateRequestedLocation(location.segment_id, location.generation)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable combined location reservation references an unrequested Catalog generation");
			}
		}
		for (const ExtentLocation &location: input.extents) {
			if (!validateRequestedLocation(location.segment_id, location.generation)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable combined location reservation references an unrequested Catalog generation");
			}
		}

		const auto validateEntityPageLocations = [&](std::span<const EntityPageLocation> locations) {
			for (const EntityPageLocation &location: locations) {
				const auto page = std::ranges::find(input.pages, location.page_id, &PageLocation::page_id);
				if (location.entity_id == 0U || !location.page_id.isValid() ||
					location.slot_id == std::numeric_limits<uint16_t>::max() || location.slot_generation == 0U ||
					page == input.pages.end()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable combined Entity Map reservation has an invalid Page Directory target");
				}
				const auto reservation = findRequestedLocation(page->segment_id, page->generation);
				// Page preflight above proves every referenced page has an exact request.
				if (reservation->kind != SegmentKind::SGK_ENTITY_PAGE_POOL) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable combined Entity Map reservation has an invalid Entity page pool");
				}
			}
		};
		validateEntityPageLocations(input.entity_page_nodes);
		validateEntityPageLocations(input.entity_page_edges);

		for (const VectorManifestLocation &manifest: input.manifests) {
			const auto page = std::ranges::find(input.pages, manifest.manifest_page_id, &PageLocation::page_id);
			if (manifest.index_id == 0U || !manifest.manifest_page_id.isValid() || !manifest.generation.isValid() ||
				page == input.pages.end()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable combined manifest reservation has an invalid manifest Page Directory target");
			}
			const auto reservation = findRequestedLocation(page->segment_id, page->generation);
			const uint8_t invalid = static_cast<uint8_t>(reservation->kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(reservation->owner_id != manifest.index_id) |
								static_cast<uint8_t>(page->generation != manifest.generation);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable combined manifest reservation has an invalid manifest Page Directory target");
			}
		}
		const Superblock &current = batch.current();
		for (const CatalogSlotReservationRequest &request: requests) {
			if (request.require_absent && catalog_.findOptional(current, request.segment_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable new-segment reservation cannot replace an existing Catalog mapping");
			}
		}
		for (const PageLocation &location: input.pages) {
			if (location.page_id.isValid() && locations_.findPageOptional(current, location.page_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable combined location reservation cannot replace an existing Page ID");
			}
		}
		for (const ExtentLocation &location: input.extents) {
			if (location.extent_id.isValid() && locations_.findExtentOptional(current, location.extent_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable combined location reservation cannot replace an existing Extent ID");
			}
		}
		for (const EntityPageLocation &location: input.entity_page_nodes) {
			if (locations_.findNodePageLocationOptional(current, location.entity_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable combined Entity Map reservation cannot replace an existing Node ID");
			}
		}
		for (const EntityPageLocation &location: input.entity_page_edges) {
			if (locations_.findEdgePageLocationOptional(current, location.entity_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable combined Entity Map reservation cannot replace an existing Edge ID");
			}
		}
		for (const EntityPageLocation &location: input.entity_page_nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: input.entity_page_edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		std::vector<CatalogEntry> entries;
		entries.reserve(requests.size());
		PhysicalSlotId nextCandidate = input.first_slot;
		for (size_t index = 0; index < requests.size(); ++index) {
			const CatalogSlotReservationRequest &request = requests[index];
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.findFirstFree(current, nextCandidate, input.last_slot);
			if (!physicalSlot.has_value()) {
				return std::nullopt;
			}
			CatalogEntry entry;
			entry.segment_id = request.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = request.kind;
			entry.owner_id = request.owner_id;
			entry.generation = request.segment_generation;
			entry.state = request.state;
			entry.committed_lsn = batch.checkpointLsn();
			batch.observeLogicalSegmentId(entry.segment_id);
			// Reserve each selected data slot before COW allocation so an appended
			// metadata pool cannot overlap a newly assigned typed segment.
			batch.reserveDataSlot(entry.physical_slot);
			entries.push_back(entry);
			if (index + 1U < requests.size()) {
				if (*physicalSlot == input.last_slot || physicalSlot->value() == std::numeric_limits<uint64_t>::max()) {
					return std::nullopt;
				}
				nextCandidate = PhysicalSlotId(physicalSlot->value() + 1U);
			}
		}
		for (const CatalogEntry &entry: entries) {
			allocator_.prepareSetAllocated(batch, entry.physical_slot, true);
		}

		catalog_.prepareUpsert(batch, entries);
		if (input.entity_page_preparer != nullptr) {
			(*input.entity_page_preparer)(entries, input.mutable_entity_pages);
		} else if (input.data_preparer != nullptr) {
			(*input.data_preparer)(entries);
		}
		if (!input.pages.empty()) {
			locations_.prepareUpsertPages(batch, input.pages);
		}
		if (!input.extents.empty()) {
			locations_.prepareUpsertExtents(batch, input.extents);
		}
		if (!input.entity_page_nodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, input.entity_page_nodes, {});
		}
		if (!input.entity_page_edges.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, input.entity_page_edges, {});
		}
		if (!input.manifests.empty()) {
			locations_.prepareReplaceVectorManifests(batch, input.manifests, {});
		}
		return entries;
	}

} // namespace graph::storage
