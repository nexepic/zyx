/**
 * @file MetadataPageFile.cpp
 * @brief Checked physical placement for relocatable Catalog metadata pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"

#include <limits>
#include <stdexcept>

namespace graph::storage {

	namespace {

		[[noreturn]] void throwMetadataPageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void validateReadHint(BufferPoolReadHint hint) {
			switch (hint) {
				case BufferPoolReadHint::BPRH_NORMAL:
				case BufferPoolReadHint::BPRH_SEQUENTIAL:
				case BufferPoolReadHint::BPRH_PREFETCH:
					return;
			}
			throwMetadataPageError(FormatError::FERR_INVALID_ENUM,
								   "relocatable metadata page read has an unknown Buffer Pool hint");
		}

	} // namespace

	MetadataPageFile::MetadataPageFile(std::shared_ptr<StorageIO> io, const Superblock &superblock) :
		io_(std::move(io)) {
		if (!io_) {
			throw std::invalid_argument("relocatable MetadataPageFile requires StorageIO");
		}
		static_cast<void>(SuperblockCodec::encode(superblock));
		data_region_offset_ = superblock.data_region_offset;
	}

	uint64_t MetadataPageFile::pageOffset(PhysicalPageRef reference) const {
		if (!reference.isValid()) {
			throwMetadataPageError(FormatError::FERR_INVALID_HEADER, "relocatable metadata page reference is invalid");
		}
		const uint64_t slotIndex = reference.physical_slot.value() - 1U;
		if (slotIndex > (std::numeric_limits<uint64_t>::max() - data_region_offset_) / RSS_SEGMENT_SIZE) {
			throwMetadataPageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								   "relocatable metadata page slot offset overflows");
		}
		const uint64_t segmentOffset = data_region_offset_ + slotIndex * RSS_SEGMENT_SIZE;
		const uint64_t frameOffset = static_cast<uint64_t>(reference.frame_index) * RSS_PAGE_SIZE;
		return segmentOffset + frameOffset;
	}

	BufferPoolPageKey MetadataPageFile::pageKey(PhysicalPageRef reference) const {
		return {pageOffset(reference), reference.generation.value()};
	}

	void MetadataPageFile::writeCatalogLeaf(PhysicalPageRef reference, const CatalogLeafPage &page) const {
		if (reference.generation != page.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable Catalog leaf generation does not match its physical reference");
		}
		writePage(reference, CatalogLeafPageCodec::encode(page));
	}

	CatalogLeafPage MetadataPageFile::readCatalogLeaf(PhysicalPageRef reference, BufferPoolReadHint hint) const {
		CatalogLeafPage page = CatalogLeafPageCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable Catalog leaf generation does not match its physical reference");
		}
		return page;
	}

	void MetadataPageFile::writeCatalogRadix(PhysicalPageRef reference, const CatalogRadixPage &page) const {
		if (page.directory_type != MetadataDirectoryType::MDT_CATALOG) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable Catalog radix has an unexpected metadata directory type");
		}
		writeMetadataRadix(reference, page);
	}

	CatalogRadixPage MetadataPageFile::readCatalogRadix(PhysicalPageRef reference, BufferPoolReadHint hint) const {
		CatalogRadixPage page = readMetadataRadix(reference, hint);
		if (page.directory_type != MetadataDirectoryType::MDT_CATALOG) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable Catalog radix has an unexpected metadata directory type");
		}
		return page;
	}

	void MetadataPageFile::writeMetadataRadix(PhysicalPageRef reference, const MetadataRadixPage &page) const {
		if (reference.generation != page.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable metadata radix generation does not match its physical reference");
		}
		writePage(reference, MetadataRadixPageCodec::encode(page));
	}

	MetadataRadixPage MetadataPageFile::readMetadataRadix(PhysicalPageRef reference, BufferPoolReadHint hint) const {
		MetadataRadixPage page = MetadataRadixPageCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable metadata radix generation does not match its physical reference");
		}
		return page;
	}

	void MetadataPageFile::writeAllocatorLeaf(PhysicalPageRef reference, const AllocatorLeafPage &page) const {
		if (reference.generation != page.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable free-space leaf generation does not match its physical reference");
		}
		writePage(reference, AllocatorLeafPageCodec::encode(page));
	}

	AllocatorLeafPage MetadataPageFile::readAllocatorLeaf(PhysicalPageRef reference, BufferPoolReadHint hint) const {
		AllocatorLeafPage page = AllocatorLeafPageCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable free-space leaf generation does not match its physical reference");
		}
		return page;
	}

	void MetadataPageFile::writeLocationLeaf(PhysicalPageRef reference, const LocationLeafPage &page) const {
		if (reference.generation != page.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable location leaf generation does not match its physical reference");
		}
		writePage(reference, LocationLeafPageCodec::encode(page));
	}

	LocationLeafPage MetadataPageFile::readLocationLeaf(PhysicalPageRef reference, BufferPoolReadHint hint) const {
		LocationLeafPage page = LocationLeafPageCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable location leaf generation does not match its physical reference");
		}
		return page;
	}

	void MetadataPageFile::writePage(PhysicalPageRef reference,
									 const std::array<std::byte, RSS_PAGE_SIZE> &bytes) const {
		io_->writeAt(pageKey(reference), bytes.data(), bytes.size());
	}

	std::array<std::byte, RSS_PAGE_SIZE> MetadataPageFile::readPage(PhysicalPageRef reference,
														BufferPoolReadHint hint) const {
		validateReadHint(hint);
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		const size_t bytesRead = io_->readAt(pageKey(reference), bytes.data(), bytes.size(), hint);
		if (bytesRead != bytes.size()) {
			throwMetadataPageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable metadata page is truncated");
		}
		return bytes;
	}

} // namespace graph::storage
