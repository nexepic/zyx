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

#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

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

		[[nodiscard]] std::optional<MetadataPageId>
		findRoutedMetadataPageId(std::span<const MetadataPageRoute> routes, PhysicalPageRef reference) {
			std::optional<MetadataPageId> result;
			for (const MetadataPageRoute &route: routes) {
				if (route.physical_reference != reference) {
					continue;
				}
			if (result.has_value()) { // authenticated route-branch traversal rejects duplicate target frames before returning routes.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
									   "routed metadata relocation aliases a physical child frame");
				}
				result = route.page_id;
			}
			return result;
		}

		[[nodiscard]] std::optional<std::vector<MetadataPageRoute>>
		collectRoutesForPhysicalChildLogicalization(MetadataPageFile &pages, const Superblock &current,
													 const MetadataRadixPage &page) {
			if (page.logical_children_enabled) { // the sole caller enters this helper only after confirming physical children need logicalization.
				return std::vector<MetadataPageRoute>{};
			}
			if (!current.usesPageDirectoryBootstrap() || !current.page_directory_root.isValid()) {
				return std::nullopt;
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
			if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwMetadataPageError(FormatError::FERR_INVALID_HEADER,
									   "routed metadata relocation has an invalid published bootstrap route");
			}
			std::vector<MetadataPageRoute> routes = MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(
					current, bootstrap.metadata_page_directory_root);
			for (const PhysicalPageRef child: page.children) {
				if (child.isValid() && !findRoutedMetadataPageId(routes, child).has_value()) {
					return std::nullopt;
				}
			}
			return routes;
		}

		void logicalizeRoutedPhysicalChildren(MetadataRadixPage &page,
												 std::span<const MetadataPageRoute> routes) {
			if (page.logical_children_enabled) {
				return;
			}
			for (size_t index = 0U; index < page.children.size(); ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					continue;
				}
				const std::optional<MetadataPageId> childId = findRoutedMetadataPageId(routes, child);
				if (!childId.has_value()) { // collectRoutesForPhysicalChildLogicalization verifies every valid child before this helper receives the route set.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
									   "routed metadata relocation has an unrouteable physical child");
				}
				page.logical_children[index] = *childId;
				page.children[index] = {};
			}
			page.logical_children_enabled = true;
			page.logical_children_resolved = false;
		}

	} // namespace

	MetadataPageFile::MetadataPageFile(std::shared_ptr<StorageIO> io, const Superblock &superblock) :
		io_(std::move(io)), superblock_(superblock) {
		if (!io_) {
			throw std::invalid_argument("relocatable MetadataPageFile requires StorageIO");
		}
		static_cast<void>(SuperblockCodec::encode(superblock));
		data_region_offset_ = superblock.data_region_offset;
	}

	void MetadataPageFile::rebindSuperblock(const Superblock &superblock) {
		static_cast<void>(SuperblockCodec::encode(superblock));
		if (superblock.data_region_offset != data_region_offset_) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable metadata page file cannot change its data-region layout");
		}
		superblock_ = superblock;
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

	CatalogRadixPage MetadataPageFile::readCatalogRadix(PhysicalPageRef reference, const Superblock &routeContext,
																								 BufferPoolReadHint hint) const {
		CatalogRadixPage page = readMetadataRadix(reference, routeContext, hint);
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
		MetadataRadixPage page = readMetadataRadixUnresolved(reference, hint);
		resolveLogicalRadixChildren(page, superblock_);
		return page;
	}

	MetadataRadixPage MetadataPageFile::readMetadataRadix(PhysicalPageRef reference, const Superblock &routeContext,
																								  BufferPoolReadHint hint) const {
		static_cast<void>(SuperblockCodec::encode(routeContext));
		if (routeContext.data_region_offset != data_region_offset_) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
									   "logical metadata radix route context changes its data-region layout");
		}
		MetadataRadixPage page = readMetadataRadixUnresolved(reference, hint);
		resolveLogicalRadixChildren(page, routeContext);
		return page;
	}

	MetadataRadixPage MetadataPageFile::readMetadataRadixUnresolved(PhysicalPageRef reference,
																	BufferPoolReadHint hint) const {
		MetadataRadixPage page = MetadataRadixPageCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable metadata radix generation does not match its physical reference");
		}
		return page;
	}

	bool MetadataPageFile::supportsRoutedMetadataPageRelocation(PhysicalPageRef source, MetadataPageId expectedPageId,
																		 const Superblock &current) const {
		if (!source.isValid() || !expectedPageId.isValid() || source.generation > current.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_HEADER,
								   "routed metadata relocation has an invalid source identity");
		}
		const std::array<std::byte, RSS_PAGE_SIZE> bytes = readPage(source, BufferPoolReadHint::BPRH_NORMAL);
		switch (LittleEndianCodec::readU32(bytes, 0U)) {
			case RSS_CATALOG_RADIX_PAGE_MAGIC: {
				const MetadataRadixPage page = MetadataRadixPageCodec::decode(bytes);
				if (page.generation != source.generation || page.generation > current.generation) { // typed page writes bind the page generation to its reference, and the caller rejects future references.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation radix identity is not current");
				}
				if (page.page_id != expectedPageId || page.committed_lsn > current.checkpoint_lsn ||
					page.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation radix identity is not current");
				}
				return collectRoutesForPhysicalChildLogicalization(const_cast<MetadataPageFile &>(*this), current, page)
						.has_value();
			}
			case RSS_CATALOG_PAGE_MAGIC: {
				const CatalogLeafPage page = CatalogLeafPageCodec::decode(bytes);
				if (page.generation != source.generation || page.generation > current.generation) { // typed page writes bind the page generation to its reference, and the caller rejects future references.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation Catalog identity is not current");
				}
				if (page.page_id != expectedPageId || page.committed_lsn > current.checkpoint_lsn) {
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation Catalog identity is not current");
				}
				return true;
			}
			case RSS_ALLOCATOR_PAGE_MAGIC: {
				const AllocatorLeafPage page = AllocatorLeafPageCodec::decode(bytes);
				if (page.generation != source.generation || page.generation > current.generation) { // typed page writes bind the page generation to its reference, and the caller rejects future references.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation allocator identity is not current");
				}
				if (page.page_id != expectedPageId || page.committed_lsn > current.checkpoint_lsn) {
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation allocator identity is not current");
				}
				return true;
			}
			case RSS_LOCATION_LEAF_MAGIC: {
				const LocationLeafPage page = LocationLeafPageCodec::decode(bytes);
				if (page.generation != source.generation || page.generation > current.generation) { // typed page writes bind the page generation to its reference, and the caller rejects future references.
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation location identity is not current");
				}
				if (page.page_id != expectedPageId || page.committed_lsn > current.checkpoint_lsn) {
					throwMetadataPageError(FormatError::FERR_INVALID_STATE,
										   "routed metadata relocation location identity is not current");
				}
				return true;
			}
			default:
				throwMetadataPageError(FormatError::FERR_INVALID_HEADER,
								   "routed metadata relocation source has an unsupported page codec");
		}
	}

	PhysicalPageRef MetadataPageFile::relocateRoutedMetadataPage(PhysicalPageRef source, MetadataPageId expectedPageId,
																				MetadataWriteBatch &batch) const {
		const Superblock &current = batch.current();
		if (!current.usesPageDirectoryBootstrap() || !current.page_directory_root.isValid()) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "routed metadata relocation requires a published bootstrap route");
		}
		const PageDirectoryBootstrap bootstrap = readPageDirectoryBootstrap(current.page_directory_root);
		if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
			!bootstrap.metadata_page_directory_root.isValid() ||
			MetadataPageRouteDirectory(const_cast<MetadataPageFile &>(*this)).find(
					current, bootstrap.metadata_page_directory_root, expectedPageId) != source) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "routed metadata relocation source is not the current PageId target");
		}
		if (!supportsRoutedMetadataPageRelocation(source, expectedPageId, current)) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "routed metadata relocation has an unrouteable physical child");
		}
		const std::array<std::byte, RSS_PAGE_SIZE> bytes = readPage(source, BufferPoolReadHint::BPRH_NORMAL);
		const PhysicalPageRef destination = batch.allocatePage();
		switch (LittleEndianCodec::readU32(bytes, 0U)) {
			case RSS_CATALOG_RADIX_PAGE_MAGIC: {
				MetadataRadixPage page = MetadataRadixPageCodec::decode(bytes);
				if (!page.logical_children_enabled) {
					const std::optional<std::vector<MetadataPageRoute>> routes =
							collectRoutesForPhysicalChildLogicalization(const_cast<MetadataPageFile &>(*this), current, page);
					if (!routes.has_value()) { // supportsRoutedMetadataPageRelocation performs the identical route-completeness check immediately before relocation.
						throwMetadataPageError(FormatError::FERR_INVALID_STATE,
											   "routed metadata relocation has an unrouteable physical child");
					}
					logicalizeRoutedPhysicalChildren(page, *routes);
				}
				page.generation = batch.generation();
				page.committed_lsn = batch.checkpointLsn();
				writeMetadataRadix(destination, page);
				break;
			}
			case RSS_CATALOG_PAGE_MAGIC: {
				CatalogLeafPage page = CatalogLeafPageCodec::decode(bytes);
				page.generation = batch.generation();
				page.committed_lsn = batch.checkpointLsn();
				writeCatalogLeaf(destination, page);
				break;
			}
			case RSS_ALLOCATOR_PAGE_MAGIC: {
				AllocatorLeafPage page = AllocatorLeafPageCodec::decode(bytes);
				page.generation = batch.generation();
				page.committed_lsn = batch.checkpointLsn();
				writeAllocatorLeaf(destination, page);
				break;
			}
			case RSS_LOCATION_LEAF_MAGIC: {
				LocationLeafPage page = LocationLeafPageCodec::decode(bytes);
				page.generation = batch.generation();
				page.committed_lsn = batch.checkpointLsn();
				writeLocationLeaf(destination, page);
				break;
			}
			default: // supportsRoutedMetadataPageRelocation authenticates the same immutable page bytes before this dispatch.
				throwMetadataPageError(FormatError::FERR_INVALID_HEADER,
								   "routed metadata relocation source has an unsupported page codec");
		}
		batch.registerMetadataPageRoute(expectedPageId, destination);
		return destination;
	}

	void MetadataPageFile::resolveLogicalRadixChildren(MetadataRadixPage &page, const Superblock &routeContext) const {
		if (!page.logical_children_enabled) {
			return;
		}
		if (!routeContext.usesPageDirectoryBootstrap() || !routeContext.page_directory_root.isValid()) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "logical metadata radix page has no physical bootstrap route context");
		}
		const PageDirectoryBootstrap bootstrap = readPageDirectoryBootstrap(routeContext.page_directory_root);
		if (!bootstrap.metadata_page_directory_root.isValid()) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
									   "logical metadata radix page is not visible through the published bootstrap route");
		}
		Superblock routeSuperblock = routeContext;
		if (bootstrap.generation != routeContext.generation) {
			// A COW candidate retains the previous bootstrap header until its final
			// route branch is written. Resolve only through that authenticated,
			// immediately preceding publication; arbitrary stale route contexts fail.
			if (bootstrap.generation.value() + 1U != routeContext.generation.value() ||
				bootstrap.committed_lsn >= routeContext.checkpoint_lsn) {
				throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "logical metadata radix page is not visible through the published bootstrap route");
			}
			routeSuperblock.generation = bootstrap.generation;
			routeSuperblock.checkpoint_lsn = bootstrap.committed_lsn;
		}
		MetadataPageRouteDirectory routes(const_cast<MetadataPageFile &>(*this));
		for (size_t index = 0; index < page.logical_children.size(); ++index) {
			const MetadataPageId childId = page.logical_children[index];
			if (!childId.isValid()) {
				continue;
			}
			page.children[index] = routes.find(routeSuperblock, bootstrap.metadata_page_directory_root, childId);
		}
		page.logical_children_resolved = true;
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

	void MetadataPageFile::writeMetadataPageRouteLeaf(PhysicalPageRef reference,
													  const MetadataPageRouteLeafPage &page) const {
		if (reference.generation != page.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "metadata PageId route leaf generation does not match its physical reference");
		}
		writePage(reference, MetadataPageRouteLeafCodec::encode(page));
	}

	MetadataPageRouteLeafPage MetadataPageFile::readMetadataPageRouteLeaf(PhysicalPageRef reference,
																		  BufferPoolReadHint hint) const {
		MetadataPageRouteLeafPage page = MetadataPageRouteLeafCodec::decode(readPage(reference, hint));
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "metadata PageId route leaf generation does not match its physical reference");
		}
		return page;
	}

	void MetadataPageFile::writePageDirectoryBootstrap(PhysicalPageRef reference,
													   const PageDirectoryBootstrap &bootstrap) const {
		if (reference.generation != bootstrap.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "Page Directory bootstrap generation does not match its physical reference");
		}
		writePage(reference, PageDirectoryBootstrapCodec::encode(bootstrap));
	}

	PageDirectoryBootstrap MetadataPageFile::readPageDirectoryBootstrap(PhysicalPageRef reference,
																		BufferPoolReadHint hint) const {
		PageDirectoryBootstrap bootstrap = PageDirectoryBootstrapCodec::decode(readPage(reference, hint));
		if (bootstrap.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "Page Directory bootstrap generation does not match its physical reference");
		}
		return bootstrap;
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
