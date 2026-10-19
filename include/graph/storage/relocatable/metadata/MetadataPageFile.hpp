/**
 * @file MetadataPageFile.hpp
 * @brief Checked file placement for isolated relocatable Catalog metadata pages.
 *
 * This component deliberately accepts only physical metadata references. It
 * does not allocate slots or interpret business records; allocation and the
 * Catalog transaction will be connected in later Stage 2 work.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/metadata/DecodedMetadataPageView.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRoute.hpp"
#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"

namespace graph::storage {

	class MetadataWriteBatch;

	class MetadataPageFile final {
	public:
		MetadataPageFile(std::shared_ptr<StorageIO> io, const Superblock &superblock);

		// Rebase a long-lived view after durable publication for logical route reads.
		void rebindSuperblock(const Superblock &superblock);

		[[nodiscard]] uint64_t pageOffset(PhysicalPageRef reference) const;

		void writeCatalogLeaf(PhysicalPageRef reference, const CatalogLeafPage &page) const;
		[[nodiscard]] CatalogLeafPage readCatalogLeaf(PhysicalPageRef reference,
													  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeCatalogRadix(PhysicalPageRef reference, const CatalogRadixPage &page) const;
		[[nodiscard]] CatalogRadixPage
		readCatalogRadix(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		[[nodiscard]] CatalogRadixPage readCatalogRadix(PhysicalPageRef reference, const Superblock &routeContext,
																								 BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeMetadataRadix(PhysicalPageRef reference, const MetadataRadixPage &page) const;
		[[nodiscard]] MetadataRadixPage
		readMetadataRadix(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Snapshot readers must supply their pinned Superblock so v5 logical
		// children are resolved through that snapshot's route branch.
		[[nodiscard]] MetadataRadixPage readMetadataRadix(PhysicalPageRef reference, const Superblock &routeContext,
																								  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Resolve selected slots of a decoded, unmodified radix page. Results follow
		// input order (duplicates allowed); absent slots yield invalid references.
		// The input page is never marked fully resolved. Callers validate descendant
		// kind/level and visibility, just as with the full read API.
		void resolveMetadataRadixChildren(const MetadataRadixPage &page, const Superblock &routeContext,
				std::span<const uint32_t> childIndices, std::span<PhysicalPageRef> results) const;
		// Inspection of a physical radix frame for writers or selected-path readers.
		// It intentionally skips
		// logical child resolution so an unpublished COW child can contribute its
		// stable page identity to its parent before route publication.
		[[nodiscard]] MetadataRadixPage
		readMetadataRadixUnresolved(PhysicalPageRef reference,
									BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
        // Immutable unresolved image; callers still validate route identity/visibility.
        [[nodiscard]] DecodedMetadataPageView<MetadataRadixPage>
        readMetadataRadixUnresolvedView(PhysicalPageRef reference,
                BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// A page-local metadata move is permitted only for a current typed route
		// target. Bootstrap plumbing stays physical and is never accepted here.
		[[nodiscard]] bool supportsRoutedMetadataPageRelocation(PhysicalPageRef source, MetadataPageId expectedPageId,
																 const Superblock &current) const;
		[[nodiscard]] PhysicalPageRef relocateRoutedMetadataPage(PhysicalPageRef source, MetadataPageId expectedPageId,
																	 MetadataWriteBatch &batch) const;

		void writeAllocatorLeaf(PhysicalPageRef reference, const AllocatorLeafPage &page) const;
		[[nodiscard]] AllocatorLeafPage
		readAllocatorLeaf(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeLocationLeaf(PhysicalPageRef reference, const LocationLeafPage &page) const;
		[[nodiscard]] LocationLeafPage
		readLocationLeaf(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeMetadataPageRouteLeaf(PhysicalPageRef reference, const MetadataPageRouteLeafPage &page) const;
		[[nodiscard]] MetadataPageRouteLeafPage
		readMetadataPageRouteLeaf(PhysicalPageRef reference,
								  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

        [[nodiscard]] DecodedMetadataPageView<CatalogLeafPage>
        readCatalogLeafView(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
        [[nodiscard]] DecodedMetadataPageView<LocationLeafPage>
        readLocationLeafView(PhysicalPageRef reference, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

        [[nodiscard]] DecodedMetadataPageView<MetadataPageRouteLeafPage>
        readMetadataPageRouteLeafView(PhysicalPageRef reference,
            BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writePageDirectoryBootstrap(PhysicalPageRef reference, const PageDirectoryBootstrap &bootstrap) const;
		[[nodiscard]] PageDirectoryBootstrap
		readPageDirectoryBootstrap(PhysicalPageRef reference,
								   BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		[[nodiscard]] const std::shared_ptr<StorageIO> &storageIO() const { return io_; }

	private:
		[[nodiscard]] BufferPoolPageKey pageKey(PhysicalPageRef reference) const;
		void writePage(PhysicalPageRef reference, const std::array<std::byte, RSS_PAGE_SIZE> &bytes) const;
		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE> readPage(PhysicalPageRef reference,
																	BufferPoolReadHint hint) const;
		void resolveLogicalRadixChildren(MetadataRadixPage &page, const Superblock &routeContext) const;
		void resolveLogicalMetadataReferences(const Superblock &routeContext,
				std::span<const MetadataPageId> ids, std::span<PhysicalPageRef> results) const;

		std::shared_ptr<StorageIO> io_;
		Superblock superblock_{};
		uint64_t data_region_offset_ = 0;
	};

} // namespace graph::storage
