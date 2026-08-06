/**
 * @file MetadataReservationCoordinator.hpp
 * @brief Atomic pre-publication coordination for Catalog-backed data slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/metadata/CatalogSlotReservation.hpp"

namespace graph::storage {

	class AllocatorDirectory;
	class CatalogTree;
	class LocationDirectory;
	class MetadataWriteBatch;

	// All logical-directory changes supplied here are prepared against one
	// MetadataWriteBatch. The caller retains the writer guard and owns the final
	// A/B Superblock publication, so a failed data preparer cannot expose a
	// partial Catalog or location mapping.
	struct CatalogReservationPublicationRequest {
		std::span<const CatalogSlotReservationRequest> reservations;
		std::span<const PageLocation> pages;
		std::span<const ExtentLocation> extents;
		std::span<const EntityPageLocation> entity_page_nodes;
		std::span<const EntityPageLocation> entity_page_edges;
		std::span<const VectorManifestLocation> manifests;
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		const CatalogSlotReservationPreparer *data_preparer = nullptr;
		const EntityPagePoolReservationPreparer *entity_page_preparer = nullptr;
		std::span<PageLocation> mutable_entity_pages;
	};

	// Coordinates the pre-publication agreement between the Catalog, free-space
	// directory, and typed logical directories. It intentionally does not own
	// locking, Superblock recovery, or publication lifecycle.
	class MetadataReservationCoordinator final {
	public:
		MetadataReservationCoordinator(CatalogTree &catalog, AllocatorDirectory &allocator,
									 LocationDirectory &locations);

		// Validates all supplied logical references, reserves first-free physical
		// slots, invokes the typed data materializer, and stages matching Catalog
		// and directory roots in `batch`. No durable root becomes visible here.
		[[nodiscard]] std::optional<std::vector<CatalogEntry>>
		prepare(MetadataWriteBatch &batch, const CatalogReservationPublicationRequest &request) const;

	private:
		CatalogTree &catalog_;
		AllocatorDirectory &allocator_;
		LocationDirectory &locations_;
	};

} // namespace graph::storage
