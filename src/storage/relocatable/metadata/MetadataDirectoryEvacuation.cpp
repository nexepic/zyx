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
#include <set>
#include <utility>

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
																	 std::set<MetadataFrameKey> &visited) {
			markEvacuationSourceReference(source, visited);
			MetadataRadixPage page = pages.readMetadataRadix(source);
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
																	 static_cast<uint16_t>(page.level - 1U), childOrdinal, visited);
					continue;
				}

				markEvacuationSourceReference(child, visited);
				if (directoryType == MetadataDirectoryType::MDT_CATALOG) {
					CatalogLeafPage leaf = pages.readCatalogLeaf(child);
					validateEvacuationLeaf(leaf, childOrdinal, current);
					leaf.page_id = batch.allocatePageId();
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeCatalogLeaf(destination, leaf);
					page.children[index] = destination;
				} else if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
					AllocatorLeafPage leaf = pages.readAllocatorLeaf(child);
					validateEvacuationLeaf(leaf, childOrdinal, current);
					leaf.page_id = batch.allocatePageId();
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeAllocatorLeaf(destination, leaf);
					page.children[index] = destination;
				} else {
					LocationLeafPage leaf = pages.readLocationLeaf(child);
					validateEvacuationLeaf(leaf, directoryType, childOrdinal, current);
					leaf.page_id = batch.allocatePageId();
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					const PhysicalPageRef destination = batch.allocatePage();
					pages.writeLocationLeaf(destination, leaf);
					page.children[index] = destination;
				}
			}

			page.page_id = batch.allocatePageId();
			page.generation = batch.generation();
			page.committed_lsn = batch.checkpointLsn();
			const PhysicalPageRef destination = batch.allocatePage();
			pages.writeMetadataRadix(destination, page);
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
		const std::array<DirectoryRoot, 14> roots = {{
				{MetadataDirectoryType::MDT_CATALOG, current.catalog_root, &MetadataWriteBatch::setCatalogRoot},
				{MetadataDirectoryType::MDT_ALLOCATOR, current.allocator_root, &MetadataWriteBatch::setAllocatorRoot},
				{MetadataDirectoryType::MDT_PAGE, current.page_directory_root, &MetadataWriteBatch::setPageDirectoryRoot},
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
		std::set<MetadataFrameKey> visited;
		for (const DirectoryRoot &directory: roots) {
			if (!directory.root.isValid()) {
				continue;
			}
			(batch.*directory.set_root)(cloneMetadataDirectoryPage(pages, batch, current, directory.root,
																			 directory.directory_type, true, 0U, 0U, visited));
		}
	}

} // namespace graph::storage
