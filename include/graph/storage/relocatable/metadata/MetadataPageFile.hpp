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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

	class MetadataPageFile final {
	public:
		MetadataPageFile(std::shared_ptr<StorageIO> io, const Superblock &superblock);

		[[nodiscard]] uint64_t pageOffset(PhysicalPageRef reference) const;

		void writeCatalogLeaf(PhysicalPageRef reference, const CatalogLeafPage &page) const;
		[[nodiscard]] CatalogLeafPage
		readCatalogLeaf(PhysicalPageRef reference,
						BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeCatalogRadix(PhysicalPageRef reference, const CatalogRadixPage &page) const;
		[[nodiscard]] CatalogRadixPage
		readCatalogRadix(PhysicalPageRef reference,
						 BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeMetadataRadix(PhysicalPageRef reference, const MetadataRadixPage &page) const;
		[[nodiscard]] MetadataRadixPage
		readMetadataRadix(PhysicalPageRef reference,
						  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeAllocatorLeaf(PhysicalPageRef reference, const AllocatorLeafPage &page) const;
		[[nodiscard]] AllocatorLeafPage
		readAllocatorLeaf(PhysicalPageRef reference,
						  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		void writeLocationLeaf(PhysicalPageRef reference, const LocationLeafPage &page) const;
		[[nodiscard]] LocationLeafPage
		readLocationLeaf(PhysicalPageRef reference,
						 BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		[[nodiscard]] const std::shared_ptr<StorageIO> &storageIO() const { return io_; }

	private:
		[[nodiscard]] BufferPoolPageKey pageKey(PhysicalPageRef reference) const;
		void writePage(PhysicalPageRef reference, const std::array<std::byte, RSS_PAGE_SIZE> &bytes) const;
		[[nodiscard]] std::array<std::byte, RSS_PAGE_SIZE>
		readPage(PhysicalPageRef reference, BufferPoolReadHint hint) const;

		std::shared_ptr<StorageIO> io_;
		uint64_t data_region_offset_ = 0;
	};

} // namespace graph::storage
