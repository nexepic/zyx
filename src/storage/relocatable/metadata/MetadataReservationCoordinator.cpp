/**
 * @file MetadataReservationCoordinator.cpp
 * @brief Atomic pre-publication coordination for Catalog-backed data slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataReservationCoordinator.hpp"

#include <cstdint>
#include <algorithm>
#include <limits>
#include <ranges>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/debug/PerfTrace.hpp"

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

		// Reservation requests are already required to be strictly ordered.  Keep
		// validation of every referenced page logarithmic instead of rescanning the
		// complete request list for each mapping in a bulk publication.
		const auto findRequestedLocation = [&requests](SegmentId segmentId, Generation generation) {
			const auto iterator = std::lower_bound(
					requests.begin(), requests.end(), segmentId,
					[](const CatalogSlotReservationRequest &request, SegmentId id) { return request.segment_id < id; });
			if (iterator == requests.end() || iterator->segment_id != segmentId ||
				iterator->segment_generation != generation) {
				return requests.end();
			}
			return iterator;
		};
		const auto validateRequestedLocation = [&findRequestedLocation, &requests](SegmentId segmentId,
																					 Generation generation) {
			return findRequestedLocation(segmentId, generation) != requests.end();
		};
		// Build one immutable page-id index for all typed references.  The old
		// nested linear scans made validation quadratic for large streamed Edge
		// publications; sorted lookup keeps memory bounded and deterministic.
		std::vector<size_t> pageOrder(input.pages.size());
		for (size_t index = 0U; index < pageOrder.size(); ++index) {
			pageOrder[index] = index;
		}
		std::ranges::sort(pageOrder, [&input](size_t left, size_t right) {
			return input.pages[left].page_id < input.pages[right].page_id;
		});
		for (size_t index = 1U; index < pageOrder.size(); ++index) {
			if (input.pages[pageOrder[index - 1U]].page_id == input.pages[pageOrder[index]].page_id) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable combined location reservation contains duplicate Page IDs");
			}
		}
		const auto findPageById = [&pageOrder, &input](PageId pageId) -> const PageLocation * {
			const auto iterator = std::lower_bound(pageOrder.begin(), pageOrder.end(), pageId,
					[&input](size_t index, PageId id) { return input.pages[index].page_id < id; });
			if (iterator == pageOrder.end() || input.pages[*iterator].page_id != pageId) {
				return nullptr;
			}
			return &input.pages[*iterator];
		};
		if (debug::PerfTrace::areValuesEnabled() && !input.pages.empty()) {
			debug::PerfTrace::addValue("native_graph.commit.metadata_reservation_page_index_builds", 1);
			debug::PerfTrace::addValue("native_graph.commit.metadata_reservation_page_index_entries",
					static_cast<int64_t>(input.pages.size()));
		}
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
				const PageLocation *page = findPageById(location.page_id);
				if (location.entity_id == 0U || !location.page_id.isValid() ||
					location.slot_id == std::numeric_limits<uint16_t>::max() || location.slot_generation == 0U ||
					page == nullptr) {
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
			const PageLocation *page = findPageById(manifest.manifest_page_id);
			if (manifest.index_id == 0U || !manifest.manifest_page_id.isValid() || !manifest.generation.isValid() ||
				page == nullptr) {
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
