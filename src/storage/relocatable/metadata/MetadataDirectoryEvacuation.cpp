/**
 * @file MetadataDirectoryEvacuation.cpp
 * @brief Fail-closed copy-on-write cloning for bootstrap metadata directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"

#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

namespace graph::storage {

	namespace {

		using MetadataFrameKey = std::pair<uint64_t, uint8_t>;

		[[noreturn]] void throwMetadataEvacuationError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void validateEvacuationVisiblePage(Generation generation, LogSequenceNumber committedLsn,
										   const Superblock &current, const char *message) {
			const uint8_t invalid = static_cast<uint8_t>(!generation.isValid()) |
								static_cast<uint8_t>(generation > current.generation) |
								static_cast<uint8_t>(!committedLsn.isValid()) |
								static_cast<uint8_t>(committedLsn > current.checkpoint_lsn);
			if (invalid != 0U) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER, message);
			}
		}

		[[nodiscard]] uint64_t evacuationRadixFactor(uint16_t level) {
			uint64_t factor = 1U;
			// MetadataRadixPageCodec rejects levels above the statically checked
			// bound, so this multiplication cannot overflow at runtime.
			for (uint16_t index = 0U; index < level; ++index) {
				factor *= RSS_CATALOG_RADIX_FANOUT;
			}
			return factor;
		}

		void markEvacuationSourceReference(PhysicalPageRef reference, std::set<MetadataFrameKey> &visited) {
			// Every caller has already checked the root or child reference. Keeping
			// this helper to one responsibility prevents an unreachable invalid-ref
			// branch from obscuring the duplicate-frame corruption check.
			if (!visited.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwMetadataEvacuationError(
						FormatError::FERR_INVALID_STATE,
						"relocatable metadata evacuation found an invalid or multiply referenced source frame");
			}
		}

		void markEvacuationPageId(MetadataPageId pageId, std::set<uint64_t> &visited) {
			if (!pageId.isValid() || !visited.emplace(pageId.value()).second) {
				throwMetadataEvacuationError(
						FormatError::FERR_INVALID_STATE,
						"relocatable metadata evacuation found an invalid or multiply referenced logical Page ID");
			}
		}

		void normalizePhysicalChildren(MetadataRadixPage &page) {
			if (!page.logical_children_enabled) {
				return;
			}
			page.logical_children.fill({});
			page.logical_children_enabled = false;
			page.logical_children_resolved = false;
		}

		[[nodiscard]] MetadataDirectoryRootOrdinal logicalRootOrdinal(MetadataDirectoryType directoryType) {
			switch (directoryType) {
				case MetadataDirectoryType::MDT_CATALOG:
					return MetadataDirectoryRootOrdinal::MDRO_CATALOG;
				case MetadataDirectoryType::MDT_ALLOCATOR:
					return MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR;
				case MetadataDirectoryType::MDT_EXTENT:
					return MetadataDirectoryRootOrdinal::MDRO_EXTENT;
				case MetadataDirectoryType::MDT_NODE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY;
				case MetadataDirectoryType::MDT_EDGE_ENTITY:
					return MetadataDirectoryRootOrdinal::MDRO_EDGE_ENTITY;
				case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
					return MetadataDirectoryRootOrdinal::MDRO_ENTITY_PROPERTY;
				case MetadataDirectoryType::MDT_INDEX_ROOT:
					return MetadataDirectoryRootOrdinal::MDRO_INDEX_ROOT;
				case MetadataDirectoryType::MDT_VECTOR_LOCATION:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_LOCATION;
				case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_VECTOR_MANIFEST;
				case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
					return MetadataDirectoryRootOrdinal::MDRO_LARGE_VALUE_MANIFEST;
				case MetadataDirectoryType::MDT_STATE:
					return MetadataDirectoryRootOrdinal::MDRO_STATE;
				case MetadataDirectoryType::MDT_RETIRE_QUEUE:
					return MetadataDirectoryRootOrdinal::MDRO_RETIRE_QUEUE;
				default:
					throwMetadataEvacuationError(FormatError::FERR_INVALID_ENUM,
													"relocatable metadata evacuation has no logical root ordinal");
			}
		}

		[[nodiscard]] PhysicalPageRef evacuationRootFor(MetadataPageFile &pages, const Superblock &current,
												MetadataDirectoryType directoryType, PhysicalPageRef physicalRoot) {
			if (!current.usesPageDirectoryBootstrap()) {
				return physicalRoot;
			}
			if (!current.page_directory_root.isValid()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
												"Page Directory bootstrap root is absent during evacuation");
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
		validateEvacuationVisiblePage(bootstrap.generation, bootstrap.committed_lsn, current,
										  "Page Directory bootstrap is newer than its Superblock during evacuation");
			if (bootstrap.generation != current.generation) {
				return physicalRoot;
			}
			const MetadataPageId rootId =
					bootstrap.metadata_directory_roots[static_cast<size_t>(logicalRootOrdinal(directoryType))];
			if (!rootId.isValid()) {
				return {};
			}
			return MetadataPageRouteDirectory(pages).find(current, bootstrap.metadata_page_directory_root, rootId);
		}

		void validateEvacuationLeaf(const CatalogLeafPage &leaf, uint64_t ordinal, const Superblock &current) {
			validateEvacuationVisiblePage(leaf.generation, leaf.committed_lsn, current,
										  "relocatable metadata evacuation found a future Catalog leaf");
			if (leaf.entries.empty()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
											 "relocatable metadata evacuation found an empty Catalog leaf");
			}
			for (const CatalogEntry &entry: leaf.entries) {
				if ((entry.segment_id.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES != ordinal) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
													 "relocatable metadata evacuation found a misrouted Catalog leaf");
				}
			}
		}

		void validateEvacuationLeaf(const AllocatorLeafPage &leaf, uint64_t ordinal, const Superblock &current) {
			validateEvacuationVisiblePage(leaf.generation, leaf.committed_lsn, current,
										  "relocatable metadata evacuation found a future Allocator leaf");
			if (ordinal > (std::numeric_limits<uint64_t>::max() - 1U) / RSS_ALLOCATOR_PAGE_SLOT_CAPACITY ||
				leaf.first_slot != PhysicalSlotId(ordinal * RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U)) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
											 "relocatable metadata evacuation found a misrouted Allocator leaf");
			}
		}

		void validateEvacuationLeaf(const LocationLeafPage &leaf, MetadataDirectoryType directoryType, uint64_t ordinal,
											 const Superblock &current) {
			validateEvacuationVisiblePage(leaf.generation, leaf.committed_lsn, current,
										  "relocatable metadata evacuation found a future location leaf");
			if (leaf.directory_type != directoryType) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
											 "relocatable metadata evacuation found a location leaf of the wrong type");
			}
			for (const LocationLeafEntry &entry: leaf.entries) {
				if ((entry.logical_id - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES != ordinal) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
													 "relocatable metadata evacuation found a misrouted location leaf");
				}
			}
		}

		[[nodiscard]] PhysicalPageRef cloneMetadataDirectoryPage(MetadataPageFile &pages, MetadataWriteBatch &batch,
										 const Superblock &current, PhysicalPageRef source,
										 MetadataDirectoryType directoryType, bool root,
										 uint16_t expectedLevel, uint64_t ordinalBase,
										 std::set<MetadataFrameKey> &visited, std::set<uint64_t> &visitedPageIds) {
			markEvacuationSourceReference(source, visited);
			MetadataRadixPage page = pages.readMetadataRadix(source);
			markEvacuationPageId(page.page_id, visitedPageIds);
			const uint8_t invalidPage =
					static_cast<uint8_t>(page.directory_type != directoryType) |
					static_cast<uint8_t>(root && page.kind != CatalogRadixPageKind::CRPK_ROOT) |
					static_cast<uint8_t>(
							!root && (page.kind != CatalogRadixPageKind::CRPK_INTERNAL || page.level != expectedLevel));
			if (invalidPage != 0U) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
											 "relocatable metadata evacuation found an invalid directory radix page");
			}
			validateEvacuationVisiblePage(page.generation, page.committed_lsn, current,
										  "relocatable metadata evacuation found a future directory radix page");

			const uint64_t factor = evacuationRadixFactor(page.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwMetadataEvacuationError(FormatError::FERR_ARITHMETIC_OVERFLOW,
													 "relocatable metadata evacuation directory route overflows");
				}
				const uint64_t childOrdinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (page.level != 0U) {
					page.children[index] =
							cloneMetadataDirectoryPage(pages, batch, current, child, directoryType, false,
															 static_cast<uint16_t>(page.level - 1U), childOrdinal, visited,
															 visitedPageIds);
					continue;
				}

				markEvacuationSourceReference(child, visited);
				if (directoryType == MetadataDirectoryType::MDT_CATALOG) {
					CatalogLeafPage leaf = pages.readCatalogLeaf(child);
					markEvacuationPageId(leaf.page_id, visitedPageIds);
					validateEvacuationLeaf(leaf, childOrdinal, current);
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeCatalogLeaf(destination, leaf);
					batch.registerMetadataPageRoute(leaf.page_id, destination);
					page.children[index] = destination;
				} else if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
					AllocatorLeafPage leaf = pages.readAllocatorLeaf(child);
					markEvacuationPageId(leaf.page_id, visitedPageIds);
					validateEvacuationLeaf(leaf, childOrdinal, current);
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeAllocatorLeaf(destination, leaf);
					batch.registerMetadataPageRoute(leaf.page_id, destination);
					page.children[index] = destination;
				} else {
					LocationLeafPage leaf = pages.readLocationLeaf(child);
					markEvacuationPageId(leaf.page_id, visitedPageIds);
					validateEvacuationLeaf(leaf, directoryType, childOrdinal, current);
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeLocationLeaf(destination, leaf);
					batch.registerMetadataPageRoute(leaf.page_id, destination);
					page.children[index] = destination;
				}
			}

			// Physical evacuation must not change the logical identity of a metadata
			// page. Old snapshots retain the source physical frame while the new
			// Superblock routes the same PageId to this replacement frame.
			// Candidate pages retain physical children until the common publication
			// gate has made their final routes visible.
			normalizePhysicalChildren(page);
			page.generation = batch.generation();
			page.committed_lsn = batch.checkpointLsn();
			const PhysicalPageRef destination = batch.allocatePage();
			pages.writeMetadataRadix(destination, page);
			batch.registerMetadataPageRoute(page.page_id, destination);
			return destination;
		}

	} // namespace

	void cloneMetadataDirectoryRootsForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch,
														  const Superblock &current) {
		struct DirectoryRoot final {
			MetadataDirectoryType directory_type;
			PhysicalPageRef root;
			void (MetadataWriteBatch::*set_root)(PhysicalPageRef);
		};
		std::array<DirectoryRoot, 12> roots = {{
				{MetadataDirectoryType::MDT_CATALOG, current.catalog_root, &MetadataWriteBatch::setCatalogRoot},
				{MetadataDirectoryType::MDT_ALLOCATOR, current.allocator_root, &MetadataWriteBatch::setAllocatorRoot},
				{MetadataDirectoryType::MDT_EXTENT, current.extent_directory_root, &MetadataWriteBatch::setExtentDirectoryRoot},
				{MetadataDirectoryType::MDT_NODE_ENTITY, current.node_entity_directory_root,
				 &MetadataWriteBatch::setNodeEntityDirectoryRoot},
				{MetadataDirectoryType::MDT_EDGE_ENTITY, current.edge_entity_directory_root,
				 &MetadataWriteBatch::setEdgeEntityDirectoryRoot},
				{MetadataDirectoryType::MDT_ENTITY_PROPERTY, current.entity_property_directory_root,
				 &MetadataWriteBatch::setEntityPropertyDirectoryRoot},
				{MetadataDirectoryType::MDT_INDEX_ROOT, current.index_root_directory_root,
				 &MetadataWriteBatch::setIndexRootDirectoryRoot},
				{MetadataDirectoryType::MDT_VECTOR_LOCATION, current.vector_location_directory_root,
				 &MetadataWriteBatch::setVectorLocationDirectoryRoot},
				{MetadataDirectoryType::MDT_VECTOR_MANIFEST, current.vector_manifest_directory_root,
				 &MetadataWriteBatch::setVectorManifestDirectoryRoot},
				{MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, current.large_value_manifest_directory_root,
				 &MetadataWriteBatch::setLargeValueManifestDirectoryRoot},
				{MetadataDirectoryType::MDT_STATE, current.state_directory_root,
				 &MetadataWriteBatch::setStateDirectoryRoot},
				{MetadataDirectoryType::MDT_RETIRE_QUEUE, current.retire_queue_root,
				 &MetadataWriteBatch::setRetireQueueRoot},
		}};
		for (DirectoryRoot &directory: roots) {
			directory.root = evacuationRootFor(pages, current, directory.directory_type, directory.root);
		}
		std::set<MetadataFrameKey> visited;
		std::set<uint64_t> visitedPageIds;
		for (const DirectoryRoot &directory: roots) {
			if (!directory.root.isValid()) {
				continue;
			}
			(batch.*directory.set_root)(cloneMetadataDirectoryPage(pages, batch, current, directory.root,
																		 directory.directory_type, true, 0U, 0U, visited, visitedPageIds));
		}
        if (current.hybrid_property_descriptor_root.isValid())
            pages.prepareHybridPropertyDescriptorRelocation(batch, current);
		if (!current.usesPageDirectoryBootstrap()) {
			if (current.page_directory_root.isValid()) {
				batch.setPageDirectoryRoot(cloneMetadataDirectoryPage(
						pages, batch, current, current.page_directory_root, MetadataDirectoryType::MDT_PAGE, true, 0U,
						0U, visited, visitedPageIds));
			}
			return;
		}
		if (!current.page_directory_root.isValid()) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
								  "Page Directory bootstrap flag has no physical header reference during evacuation");
		}
		const PageDirectoryBootstrap previous = pages.readPageDirectoryBootstrap(current.page_directory_root);
		validateEvacuationVisiblePage(previous.generation, previous.committed_lsn, current,
									  "Page Directory bootstrap is newer than its Superblock during evacuation");
		PageDirectoryBootstrap replacement = previous;
		replacement.generation = batch.generation();
		replacement.committed_lsn = batch.checkpointLsn();
		if (previous.data_page_directory_root.isValid()) {
			replacement.data_page_directory_root = cloneMetadataDirectoryPage(
					pages, batch, current, previous.data_page_directory_root, MetadataDirectoryType::MDT_PAGE, true, 0U,
					0U, visited, visitedPageIds);
		}
		MetadataPageRouteDirectory routes(pages);
		replacement.metadata_page_directory_root =
				routes.buildNextRouteBranch(batch, previous.metadata_page_directory_root);
		const PhysicalPageRef header = batch.allocatePage();
		pages.writePageDirectoryBootstrap(header, replacement);
		batch.setPageDirectoryBootstrapRoot(header);
	}

	namespace {

		struct LocallyRelocatableRouteSet final {
			std::vector<MetadataPageRoute> routes;
			std::optional<MetadataPageId> data_root_page_id;
		};

		struct MixedRoutedMetadataPool final {
			LocallyRelocatableRouteSet route_set;
			uint32_t source_frame_count = 0U;
		};

		void replaceRoutedDirectoryRoot(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
												 MetadataPageId pageId, PhysicalPageRef source,
												 PhysicalPageRef destination) {
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
			struct Root final {
				MetadataPageId page_id;
				PhysicalPageRef physical_reference;
				void (MetadataWriteBatch::*set_root)(PhysicalPageRef);
			};
			const std::array roots = {
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)],
						 current.catalog_root, &MetadataWriteBatch::setCatalogRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR)],
						 current.allocator_root, &MetadataWriteBatch::setAllocatorRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_EXTENT)],
						 current.extent_directory_root, &MetadataWriteBatch::setExtentDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_NODE_ENTITY)],
						 current.node_entity_directory_root, &MetadataWriteBatch::setNodeEntityDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_EDGE_ENTITY)],
						 current.edge_entity_directory_root, &MetadataWriteBatch::setEdgeEntityDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ENTITY_PROPERTY)],
						 current.entity_property_directory_root, &MetadataWriteBatch::setEntityPropertyDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_INDEX_ROOT)],
						 current.index_root_directory_root, &MetadataWriteBatch::setIndexRootDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_VECTOR_LOCATION)],
						 current.vector_location_directory_root, &MetadataWriteBatch::setVectorLocationDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_VECTOR_MANIFEST)],
						 current.vector_manifest_directory_root, &MetadataWriteBatch::setVectorManifestDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_LARGE_VALUE_MANIFEST)],
						 current.large_value_manifest_directory_root, &MetadataWriteBatch::setLargeValueManifestDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_STATE)],
						 current.state_directory_root, &MetadataWriteBatch::setStateDirectoryRoot},
					Root{bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_RETIRE_QUEUE)],
						 current.retire_queue_root, &MetadataWriteBatch::setRetireQueueRoot},
			};
			for (const Root &root: roots) {
				if (root.page_id != pageId) {
					continue;
				}
				if (root.physical_reference != source) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "metadata routed evacuation root disagrees with its published route");
				}
				(batch.*root.set_root)(destination);
			}
		}

		[[nodiscard]] uint32_t relocateRouteSet(MetadataPageFile &pages, MetadataWriteBatch &batch,
																	 const Superblock &current,
																	 const LocallyRelocatableRouteSet &sourceRoutes) {
			uint32_t relocated = 0U;
			for (const MetadataPageRoute &route: sourceRoutes.routes) {
				const PhysicalPageRef destination =
						pages.relocateRoutedMetadataPage(route.physical_reference, route.page_id, batch);
				if (sourceRoutes.data_root_page_id == route.page_id) {
					batch.setPendingBootstrapDataRoot(destination);
				}
				replaceRoutedDirectoryRoot(pages, batch, current, route.page_id, route.physical_reference, destination);
				++relocated;
			}
			return relocated;
		}

		[[nodiscard]] std::optional<LocallyRelocatableRouteSet>
		collectLocallyRelocatableRoutes(MetadataPageFile &pages, const Superblock &current,
																	 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		if (!current.usesPageDirectoryBootstrap() || !sourcePoolSlot.isValid() || sourceAllocatedFrames == 0U) {
			return std::nullopt;
		}
		if (!current.page_directory_root.isValid()) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
									 "metadata page-local relocation has no bootstrap header");
		}
		const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
		if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
			!bootstrap.metadata_page_directory_root.isValid()) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
									 "metadata page-local relocation has an invalid bootstrap generation");
		}
		if (current.page_directory_root.physical_slot == sourcePoolSlot ||
			bootstrap.metadata_page_directory_root.physical_slot == sourcePoolSlot) {
			return std::nullopt;
		}

		std::map<uint8_t, MetadataPageRoute> sourceFrames;
		for (const MetadataPageRoute &route:
			 MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(current, bootstrap.metadata_page_directory_root)) {
			if (route.physical_reference.physical_slot != sourcePoolSlot) {
				continue;
			}
			const uint8_t frame = route.physical_reference.frame_index;
			const uint8_t invalid = static_cast<uint8_t>(frame < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
								static_cast<uint8_t>(frame > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
								static_cast<uint8_t>((sourceAllocatedFrames &
																				(1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) == 0U) |
								static_cast<uint8_t>(!sourceFrames.emplace(frame, route).second);
			if (invalid != 0U) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "metadata page-local relocation has an invalid route target");
			}
		}
		for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			const bool allocated = (sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) != 0U;
			if (allocated != sourceFrames.contains(frame)) {
				return std::nullopt;
			}
		}
		if (sourceFrames.empty()) {
			return std::nullopt;
		}
		for (const auto &[frame, route]: sourceFrames) {
			if (!pages.supportsRoutedMetadataPageRelocation(route.physical_reference, route.page_id, current)) {
				return std::nullopt;
			}
		}
		LocallyRelocatableRouteSet result;
		result.routes.reserve(sourceFrames.size());
		for (const auto &[frame, route]: sourceFrames) {
			if (route.physical_reference == bootstrap.data_page_directory_root) {
				if (result.data_root_page_id.has_value()) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
											 "metadata page-local relocation has duplicate data-directory anchors");
				}
				result.data_root_page_id = route.page_id;
			}
			result.routes.push_back(route);
		}
		if (bootstrap.data_page_directory_root.physical_slot == sourcePoolSlot && !result.data_root_page_id.has_value()) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "metadata page-local relocation has an unrouted data-directory anchor");
		}
			return result;
		}

		[[nodiscard]] std::optional<MixedRoutedMetadataPool>
		collectMixedRoutedMetadataPool(MetadataPageFile &pages, const Superblock &current,
																PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
			if (!current.usesPageDirectoryBootstrap() || !sourcePoolSlot.isValid() || sourceAllocatedFrames == 0U ||
				!current.page_directory_root.isValid()) {
				return std::nullopt;
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
			if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
										 "mixed metadata-pool evacuation has an invalid published bootstrap route");
			}

			enum class SourceFrameKind : uint8_t { SFK_ROUTE_TARGET, SFK_ROUTE_BRANCH, SFK_BOOTSTRAP_HEADER };
			std::map<uint8_t, SourceFrameKind> sourceFrames;
			const auto registerFrame = [&](PhysicalPageRef reference, SourceFrameKind kind) {
				const uint8_t frame = reference.frame_index;
				if (reference.physical_slot != sourcePoolSlot || frame < RSS_PAGE_POOL_FIRST_DATA_FRAME ||
					frame > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
					(sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) == 0U ||
					!sourceFrames.emplace(frame, kind).second) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "mixed metadata-pool evacuation has an invalid or aliased source frame");
				}
			};

			LocallyRelocatableRouteSet routeSet;
			for (const MetadataPageRoute &route:
				 MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(current, bootstrap.metadata_page_directory_root)) {
				if (route.physical_reference.physical_slot != sourcePoolSlot) {
					continue;
				}
				registerFrame(route.physical_reference, SourceFrameKind::SFK_ROUTE_TARGET);
				if (!pages.supportsRoutedMetadataPageRelocation(route.physical_reference, route.page_id, current)) {
					return std::nullopt;
				}
				if (route.physical_reference == bootstrap.data_page_directory_root) {
					routeSet.data_root_page_id = route.page_id;
				}
				routeSet.routes.push_back(route);
			}
			for (const PhysicalPageRef frame:
				 MetadataPageRouteDirectory(pages).collectRouteBranchFrames(current, bootstrap.metadata_page_directory_root)) {
				if (frame.physical_slot == sourcePoolSlot) {
					registerFrame(frame, SourceFrameKind::SFK_ROUTE_BRANCH);
				}
			}
			if (current.page_directory_root.physical_slot == sourcePoolSlot) {
				registerFrame(current.page_directory_root, SourceFrameKind::SFK_BOOTSTRAP_HEADER);
			}
			if (bootstrap.data_page_directory_root.isValid() &&
				bootstrap.data_page_directory_root.physical_slot == sourcePoolSlot && !routeSet.data_root_page_id.has_value()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "mixed metadata-pool evacuation has an unrouted data-directory anchor");
			}
			for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
				const bool allocated = (sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) != 0U;
				if (allocated != sourceFrames.contains(frame)) {
					return std::nullopt;
				}
			}
			bool hasTargets = false;
			bool hasPlumbing = false;
			for (const auto &[frame, kind]: sourceFrames) {
				static_cast<void>(frame);
				hasTargets |= kind == SourceFrameKind::SFK_ROUTE_TARGET;
				hasPlumbing |= kind != SourceFrameKind::SFK_ROUTE_TARGET;
			}
			if (!hasTargets || !hasPlumbing) {
				return std::nullopt;
			}
			return MixedRoutedMetadataPool{std::move(routeSet), static_cast<uint32_t>(sourceFrames.size())};
		}

		[[nodiscard]] std::optional<uint32_t>
		collectRefreshableRouteBranchFrames(MetadataPageFile &pages, const Superblock &current,
															PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
			if (!current.usesPageDirectoryBootstrap() || !sourcePoolSlot.isValid() || sourceAllocatedFrames == 0U ||
				!current.page_directory_root.isValid()) {
				return std::nullopt;
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
			if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
										 "metadata route-branch evacuation has an invalid bootstrap generation");
			}
			if (current.page_directory_root.physical_slot == sourcePoolSlot ||
				(bootstrap.data_page_directory_root.isValid() &&
				 bootstrap.data_page_directory_root.physical_slot == sourcePoolSlot)) {
				return std::nullopt;
			}

			std::map<uint8_t, PhysicalPageRef> sourceFrames;
			for (const PhysicalPageRef reference:
				 MetadataPageRouteDirectory(pages).collectRouteBranchFrames(current, bootstrap.metadata_page_directory_root)) {
				if (reference.physical_slot != sourcePoolSlot) {
					continue;
				}
				const uint8_t frame = reference.frame_index;
				if (frame < RSS_PAGE_POOL_FIRST_DATA_FRAME || frame > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
											 "metadata route-branch evacuation found an out-of-range physical frame");
				}
				if ((sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) == 0U) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
											 "metadata route-branch evacuation found an unallocated physical frame");
				}
				if (!sourceFrames.emplace(frame, reference).second) {
					throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
											 "metadata route-branch evacuation aliases its physical frames");
				}
			}
			for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
				const bool allocated = (sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) != 0U;
				if (allocated != sourceFrames.contains(frame)) {
					return std::nullopt;
				}
			}
			if (sourceFrames.empty()) {
				return std::nullopt;
			}
			return static_cast<uint32_t>(sourceFrames.size());
		}

		[[nodiscard]] std::optional<uint32_t>
		collectRefreshableBootstrapHeaderFrame(MetadataPageFile &pages, const Superblock &current,
															 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
			if (!current.usesPageDirectoryBootstrap() || !current.page_directory_root.isValid() ||
				!sourcePoolSlot.isValid() || current.page_directory_root.physical_slot != sourcePoolSlot) {
				return std::nullopt;
			}
			const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
			if (bootstrap.generation != current.generation || bootstrap.committed_lsn != current.checkpoint_lsn ||
				!bootstrap.metadata_page_directory_root.isValid()) {
				throwMetadataEvacuationError(FormatError::FERR_INVALID_HEADER,
										 "metadata bootstrap-header evacuation has an invalid published header");
			}
			const uint8_t frame = current.page_directory_root.frame_index;
			if (frame < RSS_PAGE_POOL_FIRST_DATA_FRAME || frame > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
				(sourceAllocatedFrames & (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) == 0U ||
				sourceAllocatedFrames != (1U << (frame - RSS_PAGE_POOL_FIRST_DATA_FRAME))) {
				return std::nullopt;
			}
			// Verify the route branch before requesting a replacement header; no
			// zero-page success may bypass its normal publication validation.
			static_cast<void>(MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(
					current, bootstrap.metadata_page_directory_root));
			return 1U;
		}

	} // namespace

	bool canRelocateRoutedMetadataPoolPages(MetadataPageFile &pages, const Superblock &current,
															  PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		return collectLocallyRelocatableRoutes(pages, current, sourcePoolSlot, sourceAllocatedFrames).has_value();
	}

	std::optional<uint32_t>
	relocateRoutedMetadataPoolPages(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
																			 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		if (batch.current() != current) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
									 "metadata page-local relocation batch does not match its published Superblock");
		}
		const std::optional<LocallyRelocatableRouteSet> sourceRoutes =
				collectLocallyRelocatableRoutes(pages, current, sourcePoolSlot, sourceAllocatedFrames);
		if (!sourceRoutes.has_value()) {
			return std::nullopt;
		}
		return relocateRouteSet(pages, batch, current, *sourceRoutes);
	}

	bool canEvacuateMixedRoutedMetadataPool(MetadataPageFile &pages, const Superblock &current,
													  PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		return collectMixedRoutedMetadataPool(pages, current, sourcePoolSlot, sourceAllocatedFrames).has_value();
	}

	std::optional<uint32_t>
	evacuateMixedRoutedMetadataPool(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
												 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		if (batch.current() != current) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "mixed metadata-pool evacuation batch does not match its published Superblock");
		}
		const std::optional<MixedRoutedMetadataPool> source =
				collectMixedRoutedMetadataPool(pages, current, sourcePoolSlot, sourceAllocatedFrames);
		if (!source.has_value()) {
			return std::nullopt;
		}
		static_cast<void>(relocateRouteSet(pages, batch, current, source->route_set));
		batch.requestBootstrapRouteRefresh();
		return source->source_frame_count;
	}

	bool canRefreshMetadataRouteBranchForEvacuation(MetadataPageFile &pages, const Superblock &current,
																  PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		return collectRefreshableRouteBranchFrames(pages, current, sourcePoolSlot, sourceAllocatedFrames).has_value();
	}

	std::optional<uint32_t>
	refreshMetadataRouteBranchForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
																PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		if (batch.current() != current) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "metadata route-branch evacuation batch does not match its published Superblock");
		}
		const std::optional<uint32_t> refreshed =
				collectRefreshableRouteBranchFrames(pages, current, sourcePoolSlot, sourceAllocatedFrames);
		if (!refreshed.has_value()) {
			return std::nullopt;
		}
		batch.requestBootstrapRouteRefresh();
		return refreshed;
	}

	bool canRefreshBootstrapHeaderForEvacuation(MetadataPageFile &pages, const Superblock &current,
																 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		return collectRefreshableBootstrapHeaderFrame(pages, current, sourcePoolSlot, sourceAllocatedFrames).has_value();
	}

	std::optional<uint32_t>
	refreshBootstrapHeaderForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
															PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames) {
		if (batch.current() != current) {
			throwMetadataEvacuationError(FormatError::FERR_INVALID_STATE,
										 "metadata bootstrap-header evacuation batch does not match its published Superblock");
		}
		const std::optional<uint32_t> refreshed =
				collectRefreshableBootstrapHeaderFrame(pages, current, sourcePoolSlot, sourceAllocatedFrames);
		if (!refreshed.has_value()) {
			return std::nullopt;
		}
		batch.requestBootstrapRouteRefresh();
		return refreshed;
	}

} // namespace graph::storage
