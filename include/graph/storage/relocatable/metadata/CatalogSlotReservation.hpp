/**
 * @file CatalogSlotReservation.hpp
 * @brief Shared request and callback contracts for atomic Catalog-slot reservation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

	struct CatalogSlotReservationRequest {
		SegmentId segment_id{};
		SegmentKind kind = SegmentKind::SGK_NONE;
		uint64_t owner_id = 0;
		Generation segment_generation{};
		SegmentState state = SegmentState::SGS_FREE;
		// Allocation callers set this so a new logical Segment ID can never
		// silently replace an existing durable mapping.
		bool require_absent = false;
	};

	struct CatalogSlotReservation {
		Superblock superblock;
		CatalogEntry entry;
	};

	struct CatalogSlotReservations {
		Superblock superblock;
		std::vector<CatalogEntry> entries;
	};

	// Runs while the metadata writer guard is held, after every requested slot
	// has been reserved in the candidate roots and before those roots can become
	// Superblock-recoverable. The callback must write and validate the reserved data
	// through the same data StorageIO before it returns.
	using CatalogSlotReservationPreparer = std::function<void(std::span<const CatalogEntry>)>;
	// Entity pages receive their physical frame only while the metadata writer
	// owns the fresh pool. The callback must persist the page bytes and set each
	// Page Directory frame before Catalog, Page Directory, and Entity Map roots
	// can be published together.
	using EntityPagePoolReservationPreparer =
			std::function<void(std::span<const CatalogEntry>, std::span<PageLocation>)>;

} // namespace graph::storage
