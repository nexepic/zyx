/**
 * @file MetadataPoolOwnership.hpp
 * @brief Durable Catalog and allocator ownership for metadata page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

namespace graph::storage {

	class AllocatorDirectory;
	class CatalogTree;
	class MetadataPagePoolAllocator;
	class MetadataWriteBatch;
	struct Superblock;

	// Coordinates the three authoritative views of metadata-pool ownership:
	// in-memory pool headers, the immutable Catalog, and the Allocator bitmap.
	// The metadata-store facade owns publication, so this component cannot make
	// a partially reconciled ownership transition visible.
	class MetadataPoolOwnership final {
	public:
		MetadataPoolOwnership(CatalogTree &catalog, AllocatorDirectory &allocator,
						  MetadataPagePoolAllocator &metadataPagePool, bool readOnly);

		// Rehydrates Catalog-owned empty pools after root-directed discovery and
		// rejects any disagreement between durable Catalog and pool headers.
		void restoreCatalogOwnership(const Superblock &current);
		// Adds missing ownership records for pools created in `batch`, or rejects
		// any published pool that cannot be reconciled in the same COW generation.
		void preparePublication(MetadataWriteBatch &batch);
		// Reclaims only unreachable, empty unpublished pools on writable recovery.
		// A read-only open fails rather than attempting an on-disk repair.
		void verifyAfterOpen(const Superblock &current);

	private:
		CatalogTree &catalog_;
		AllocatorDirectory &allocator_;
		MetadataPagePoolAllocator &metadata_page_pool_;
		bool read_only_ = false;
	};

} // namespace graph::storage
