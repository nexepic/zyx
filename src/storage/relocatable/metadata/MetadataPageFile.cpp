/**
 * @file MetadataPageFile.cpp
 * @brief Checked physical placement for relocatable Catalog metadata pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"

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
			const auto bootstrapView = pages.readPageDirectoryBootstrapView(current.page_directory_root);
            const auto &bootstrap = *bootstrapView;
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
		io_(std::move(io)), superblock_(superblock), database_id_(superblock.database_id) {
		if (!io_) {
			throw std::invalid_argument("relocatable MetadataPageFile requires StorageIO");
		}
		SuperblockCodec::validate(superblock);
		data_region_offset_ = superblock.data_region_offset;
	}

	void MetadataPageFile::rebindSuperblock(const Superblock &superblock) {
		SuperblockCodec::validate(superblock);
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<CatalogLeafPage>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.catalog", sizeof(CatalogLeafPage));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.catalog");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.catalog", 1);
        std::optional<utils::OwnedAllocationScope> allocation;
        if (!utils::OwnedAllocationScope::active()) {
            if (auto *decodedContext = MetadataPageReadContext::current(*io_)) allocation.emplace(decodedContext->memoryOwner());
        }
		CatalogLeafPage page = CatalogLeafPageCodec::decode(bytes);
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable Catalog leaf generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, page, page.entries.capacity() * sizeof(page.entries.front()));
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
		if (auto *context = MetadataPageReadContext::current(*io_);
			context == nullptr || !context->authenticates(routeContext)) {
			SuperblockCodec::validate(routeContext);
		}
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<MetadataRadixPage>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.radix", sizeof(MetadataRadixPage));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.radix");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.radix", 1);
		MetadataRadixPage page = MetadataRadixPageCodec::decode(bytes);
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable metadata radix generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, page);
		return page;
	}

    DecodedMetadataPageView<MetadataRadixPage>
    MetadataPageFile::readMetadataRadixUnresolvedView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<MetadataRadixPage>(reference))
                return DecodedMetadataPageView<MetadataRadixPage>(std::move(page));
        }
        return DecodedMetadataPageView<MetadataRadixPage>(readMetadataRadixUnresolved(reference, hint));
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
		const auto bootstrapView =  readPageDirectoryBootstrapView(current.page_directory_root);
            const auto &bootstrap = *bootstrapView;
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
		        std::optional<utils::OwnedAllocationScope> allocation;
        if (!utils::OwnedAllocationScope::active()) {
            if (auto *decodedContext = MetadataPageReadContext::current(*io_)) allocation.emplace(decodedContext->memoryOwner());
        }
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
		        std::optional<utils::OwnedAllocationScope> allocation;
        if (!utils::OwnedAllocationScope::active()) {
            if (auto *decodedContext = MetadataPageReadContext::current(*io_)) allocation.emplace(decodedContext->memoryOwner());
        }
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

	void MetadataPageFile::resolveLogicalMetadataReferences(const Superblock &routeContext,
			std::span<const MetadataPageId> ids, std::span<PhysicalPageRef> results) const {
		if (!routeContext.usesPageDirectoryBootstrap() || !routeContext.page_directory_root.isValid()) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "logical metadata radix page has no physical bootstrap route context");
		}
		const auto bootstrapView =  readPageDirectoryBootstrapView(routeContext.page_directory_root);
            const auto &bootstrap = *bootstrapView;
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
		routes.findMany(routeSuperblock, bootstrap.metadata_page_directory_root, ids, results);
	}

	void MetadataPageFile::resolveMetadataRadixChildren(const MetadataRadixPage &page,
			const Superblock &routeContext, std::span<const uint32_t> childIndices,
			std::span<PhysicalPageRef> results) const {
		if (auto *context = MetadataPageReadContext::current(*io_);
			context == nullptr || !context->authenticates(routeContext)) {
			SuperblockCodec::validate(routeContext);
		}
		if (routeContext.data_region_offset != data_region_offset_ ||
			routeContext.database_id != superblock_.database_id ||
			childIndices.size() != results.size() || childIndices.size() > RSS_CATALOG_RADIX_FANOUT) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
					"selected metadata radix resolution has an invalid context or result shape");
		}
		for (const uint32_t index : childIndices) {
			if (index >= RSS_CATALOG_RADIX_FANOUT) {
				throwMetadataPageError(FormatError::FERR_INVALID_STATE,
						"selected metadata radix child index is outside the page");
			}
		}
		if (childIndices.empty()) return;
		debug::PerfTrace::addValue("native_graph.read.radix_requested_children", childIndices.size());
		std::array<MetadataPageId, RSS_CATALOG_RADIX_FANOUT> ids{};
		std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT> references{};
		size_t count = 0U;
		if (page.logical_children_enabled) {
			for (const uint32_t index : childIndices) {
				if (page.logical_children[index].isValid()) ids[count++] = page.logical_children[index];
			}
			// Authenticate even an empty selected slot on a logical page. The full
			// API also requires its bootstrap route to be valid in this case.
			resolveLogicalMetadataReferences(routeContext, std::span(ids).first(count),
					std::span(references).first(count));
		}
		debug::PerfTrace::addValue("native_graph.read.radix_resolved_children", count);
		size_t resolved = 0U;
		for (size_t position = 0U; position < childIndices.size(); ++position) {
			const uint32_t index = childIndices[position];
			results[position] = !page.logical_children_enabled ? page.children[index] :
					(page.logical_children[index].isValid() ? references[resolved++] : PhysicalPageRef{});
		}
	}

	void MetadataPageFile::resolveLogicalRadixChildren(MetadataRadixPage &page, const Superblock &routeContext) const {
		if (!page.logical_children_enabled) return;
		std::array<MetadataPageId, RSS_CATALOG_RADIX_FANOUT> childIds{};
		std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT> childReferences{};
		size_t childCount = 0U;
		for (const MetadataPageId childId : page.logical_children) {
			if (childId.isValid()) childIds[childCount++] = childId;
		}
		resolveLogicalMetadataReferences(routeContext, std::span(childIds).first(childCount),
				std::span(childReferences).first(childCount));
		size_t resolvedIndex = 0U;
		for (size_t index = 0; index < page.logical_children.size(); ++index) {
			if (page.logical_children[index].isValid()) page.children[index] = childReferences[resolvedIndex++];
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<AllocatorLeafPage>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.allocator", sizeof(AllocatorLeafPage));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.allocator");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.allocator", 1);
		AllocatorLeafPage page = AllocatorLeafPageCodec::decode(bytes);
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable free-space leaf generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, page);
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<LocationLeafPage>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.location", sizeof(LocationLeafPage));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.location");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.location", 1);
        std::optional<utils::OwnedAllocationScope> allocation;
        if (!utils::OwnedAllocationScope::active()) {
            if (auto *decodedContext = MetadataPageReadContext::current(*io_)) allocation.emplace(decodedContext->memoryOwner());
        }
		LocationLeafPage page = LocationLeafPageCodec::decode(bytes);
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "relocatable location leaf generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, page, page.entries.capacity() * sizeof(page.entries.front()));
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<MetadataPageRouteLeafPage>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.route_leaf", sizeof(MetadataPageRouteLeafPage));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.route_leaf");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.route_leaf", 1);
        std::optional<utils::OwnedAllocationScope> allocation;
        if (!utils::OwnedAllocationScope::active()) {
            if (auto *decodedContext = MetadataPageReadContext::current(*io_)) allocation.emplace(decodedContext->memoryOwner());
        }
		MetadataPageRouteLeafPage page = MetadataPageRouteLeafCodec::decode(bytes);
		if (page.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "metadata PageId route leaf generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, page, page.routes.capacity() * sizeof(page.routes.front()));
		return page;
	}

    DecodedMetadataPageView<CatalogLeafPage>
    MetadataPageFile::readCatalogLeafView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<CatalogLeafPage>(reference))
                return DecodedMetadataPageView<CatalogLeafPage>(std::move(page));
        }
        return DecodedMetadataPageView<CatalogLeafPage>(readCatalogLeaf(reference, hint));
    }

    DecodedMetadataPageView<LocationLeafPage>
    MetadataPageFile::readLocationLeafView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<LocationLeafPage>(reference))
                return DecodedMetadataPageView<LocationLeafPage>(std::move(page));
        }
        return DecodedMetadataPageView<LocationLeafPage>(readLocationLeaf(reference, hint));
    }

    DecodedMetadataPageView<MetadataPageRouteLeafPage>
    MetadataPageFile::readMetadataPageRouteLeafView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<MetadataPageRouteLeafPage>(reference))
                return DecodedMetadataPageView<MetadataPageRouteLeafPage>(std::move(page));
        }
        return DecodedMetadataPageView<MetadataPageRouteLeafPage>(readMetadataPageRouteLeaf(reference, hint));
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
		validateReadHint(hint);
		auto *readContext = MetadataPageReadContext::current(*io_);
		if (readContext != nullptr) {
			if (auto cached = readContext->find<PageDirectoryBootstrap>(reference)) {
                debug::PerfTrace::addValue("native_graph.read.metadata_copy_fixed_bytes.bootstrap", sizeof(PageDirectoryBootstrap));
                return std::move(*cached);
            }
		}
		const auto bytes = readPage(reference, hint);
		debug::ScopedPerfTimer decodeTimer("native_graph.read.metadata_decode.bootstrap");
		debug::PerfTrace::addValue("native_graph.read.metadata_decodes.bootstrap", 1);
		PageDirectoryBootstrap bootstrap = PageDirectoryBootstrapCodec::decode(bytes);
		if (bootstrap.generation != reference.generation) {
			throwMetadataPageError(FormatError::FERR_INVALID_STATE,
								   "Page Directory bootstrap generation does not match its physical reference");
		}
		if (readContext != nullptr) readContext->put(reference, bootstrap);
		return bootstrap;
	}

    DecodedMetadataPageView<AllocatorLeafPage>
    MetadataPageFile::readAllocatorLeafView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<AllocatorLeafPage>(reference))
                return DecodedMetadataPageView<AllocatorLeafPage>(std::move(page));
        }
        return DecodedMetadataPageView<AllocatorLeafPage>(readAllocatorLeaf(reference, hint));
    }

    DecodedMetadataPageView<PageDirectoryBootstrap>
    MetadataPageFile::readPageDirectoryBootstrapView(PhysicalPageRef reference, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (auto *context = MetadataPageReadContext::current(*io_)) {
            if (auto page = context->findView<PageDirectoryBootstrap>(reference))
                return DecodedMetadataPageView<PageDirectoryBootstrap>(std::move(page));
        }
        return DecodedMetadataPageView<PageDirectoryBootstrap>(readPageDirectoryBootstrap(reference, hint));
    }

    HybridPropertyDescriptorPage MetadataPageFile::readHybridPropertyDescriptor(PhysicalPageRef reference,
        const Superblock &snapshot, BufferPoolReadHint hint) const {
        validateReadHint(hint);
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED || snapshot.database_id != database_id_ ||
            snapshot.data_region_offset != data_region_offset_ || reference != snapshot.hybrid_property_descriptor_root)
            throwMetadataPageError(FormatError::FERR_INVALID_STATE, "hybrid descriptor snapshot belongs to another format or file");
        const auto bytes = readPage(reference, hint);
        debug::PerfTrace::addValue("native_graph.read.metadata_decodes.hybrid_descriptor", 1);
        auto page = HybridPropertyDescriptorCodec::decode(bytes, snapshot.database_id, reference.generation,
            snapshot.generation, snapshot.checkpoint_lsn);
        // Nonempty descriptors are readable in the isolated format. Their
        // actual namespaces/roots and field pages are authenticated by the
        // pinned field reader. Generic publication remains rejected below.
        for (size_t base : {size_t(0), size_t(4)}) {
            const auto &roots = page.descriptor.roots;
            if ((roots[base].page_id.isValid() || roots[base + 1].page_id.isValid() || roots[base + 2].page_id.isValid()) &&
                !roots[base + 3].page_id.isValid())
                throwMetadataPageError(FormatError::FERR_INVALID_STATE, "hybrid field namespaces have no group root");
            // Only the isolated compact cohort can authenticate column
            // memberships through group-owned matrices instead of OwnerMap.
            // Extension selectors always retain their explicit owner root.
            if ((roots[base + 2].page_id.isValid() ||
                (roots[base + 1].page_id.isValid() &&
                    !(snapshot.flags & RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP))) && !roots[base].page_id.isValid())
                throwMetadataPageError(FormatError::FERR_INVALID_STATE, "hybrid field source namespace has no owner root");
        }
        return page;
    }

    void MetadataPageFile::prepareHybridPropertyDescriptor(MetadataWriteBatch &batch,
        const HybridPropertyDescriptor &descriptor) const {
        prepareHybridPropertyDescriptorImpl(batch, descriptor, false);
    }

    void MetadataPageFile::prepareHybridPropertyDescriptorImpl(MetadataWriteBatch &batch,
        const HybridPropertyDescriptor &descriptor, bool nativeCandidate) const {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED || batch.candidate().database_id != database_id_ ||
            batch.candidate().data_region_offset != data_region_offset_ || descriptor.database_id != database_id_ ||
            descriptor.semantic_generation != batch.generation() ||
            batch.candidate().hybrid_property_descriptor_root != batch.current().hybrid_property_descriptor_root)
            throwMetadataPageError(FormatError::FERR_INVALID_STATE, "hybrid descriptor publication has a foreign or stale semantic identity");
        if (!nativeCandidate && std::ranges::any_of(descriptor.roots, [](const auto &root) { return root.page_id.isValid(); }))
            throwMetadataPageError(FormatError::FERR_UNSUPPORTED_VERSION, "nonempty hybrid property publication is not installed");
        const HybridPropertyDescriptorPage page{batch.generation(), batch.checkpointLsn(), descriptor};
        // Validate/encode before allocating a frame or changing the candidate root.
        const auto bytes = HybridPropertyDescriptorCodec::encode(page);
        const auto reference = batch.allocatePage();
        writePage(reference, bytes);
        batch.setHybridPropertyDescriptorRoot(reference);
    }

    void MetadataPageFile::prepareHybridPropertyDescriptorRelocation(MetadataWriteBatch &batch,
        const Superblock &snapshot) const {
        if (batch.current() != snapshot || snapshot.database_id != database_id_ ||
            batch.candidate().hybrid_property_descriptor_root != snapshot.hybrid_property_descriptor_root)
            throwMetadataPageError(FormatError::FERR_INVALID_STATE, "hybrid descriptor relocation has a foreign publication base");
        auto page = readHybridPropertyDescriptor(snapshot.hybrid_property_descriptor_root, snapshot);
        page.generation = batch.generation(); page.committed_lsn = batch.checkpointLsn();
        const auto bytes = HybridPropertyDescriptorCodec::encode(page);
        const auto reference = batch.allocatePage();
        writePage(reference, bytes);
        batch.setHybridPropertyDescriptorRoot(reference);
    }

	void MetadataPageFile::writePage(PhysicalPageRef reference,
									 const std::array<std::byte, RSS_PAGE_SIZE> &bytes) const {
		io_->writeAt(pageKey(reference), bytes.data(), bytes.size());
	}

	std::array<std::byte, RSS_PAGE_SIZE> MetadataPageFile::readPage(PhysicalPageRef reference,
																	BufferPoolReadHint hint) const {
		debug::ScopedPerfTimer acquisitionTimer("native_graph.read.metadata_acquisition");
		validateReadHint(hint);
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		const size_t bytesRead = io_->readAt(pageKey(reference), bytes.data(), bytes.size(), hint);
		if (bytesRead != bytes.size()) {
			throwMetadataPageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable metadata page is truncated");
		}
		debug::PerfTrace::addValue("native_graph.read.metadata_pages_read", 1);
		return bytes;
	}

} // namespace graph::storage
