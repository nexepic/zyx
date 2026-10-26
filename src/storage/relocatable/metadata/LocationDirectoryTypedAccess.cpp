/**
 * @file LocationDirectory.cpp
 * @brief Explicit codecs and COW trees for Page and Extent directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include <memory_resource>
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {
namespace {
[[noreturn]] void throwLocationDirectoryError(FormatError error, const char *message) { throw FormatException(error, message); }
}
	PageLocation LocationDirectory::findPage(const Superblock &superblock, PageId pageId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_PAGE, pageId.value());
		return {pageId, entry.segment_id, entry.first_page, entry.generation, entry.free_bytes};
	}

	std::optional<PageLocation> LocationDirectory::findPageOptional(const Superblock &superblock, PageId pageId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_PAGE, pageId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return PageLocation{pageId, entry->segment_id, entry->first_page, entry->generation, entry->free_bytes};
	}

	ExtentLocation LocationDirectory::findExtent(const Superblock &superblock, ExtentId extentId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_EXTENT, extentId.value());
		return {extentId, entry.segment_id, entry.first_page, entry.page_count, entry.generation};
	}

	std::optional<ExtentLocation> LocationDirectory::findExtentOptional(const Superblock &superblock,
																		ExtentId extentId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_EXTENT, extentId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return ExtentLocation{extentId, entry->segment_id, entry->first_page, entry->page_count, entry->generation};
	}

	EntityPageLocation LocationDirectory::findNodePageLocation(const Superblock &superblock, uint64_t nodeId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, nodeId);
		return {nodeId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	EntityPageLocation LocationDirectory::findEdgePageLocation(const Superblock &superblock, uint64_t edgeId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, edgeId);
		return {edgeId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<EntityPageLocation> LocationDirectory::findNodePageLocationOptional(const Superblock &superblock,
																					  uint64_t nodeId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, nodeId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPageLocation{nodeId, PageId(entry->segment_id.value()), entry->first_page,
								  static_cast<uint32_t>(entry->generation.value())};
	}

	std::optional<EntityPageLocation> LocationDirectory::findEdgePageLocationOptional(const Superblock &superblock,
																						  uint64_t edgeId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, edgeId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPageLocation{edgeId, PageId(entry->segment_id.value()), entry->first_page,
								  static_cast<uint32_t>(entry->generation.value())};
	}

	EntityPropertyLocation LocationDirectory::findEntityProperty(const Superblock &superblock,
																 const EntityPropertyKey &key) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, key.directoryId());
		return {key, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<EntityPropertyLocation>
	LocationDirectory::findEntityPropertyOptional(const Superblock &superblock, const EntityPropertyKey &key) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, key.directoryId());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return EntityPropertyLocation{key, PageId(entry->segment_id.value()), entry->first_page,
									  static_cast<uint32_t>(entry->generation.value())};
	}

	StateLocation LocationDirectory::findState(const Superblock &superblock, uint64_t stateId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_STATE, stateId);
		return {stateId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<StateLocation> LocationDirectory::findStateOptional(const Superblock &superblock,
																	  uint64_t stateId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_STATE, stateId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return StateLocation{stateId, PageId(entry->segment_id.value()), entry->first_page,
							 static_cast<uint32_t>(entry->generation.value())};
	}

	IndexRootLocation LocationDirectory::findIndexRoot(const Superblock &superblock, uint64_t indexId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, indexId);
		return {indexId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<IndexRootLocation> LocationDirectory::findIndexRootOptional(const Superblock &superblock,
																			  uint64_t indexId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, indexId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return IndexRootLocation{indexId, PageId(entry->segment_id.value()), entry->generation};
	}

	VectorLocation LocationDirectory::findVectorLocation(const Superblock &superblock, uint64_t vectorId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, vectorId);
		return {vectorId, PageId(entry.segment_id.value()), entry.first_page,
				static_cast<uint32_t>(entry.generation.value())};
	}

	std::optional<VectorLocation> LocationDirectory::findVectorLocationOptional(const Superblock &superblock,
																				uint64_t vectorId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, vectorId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return VectorLocation{vectorId, PageId(entry->segment_id.value()), entry->first_page,
							  static_cast<uint32_t>(entry->generation.value())};
	}

	VectorManifestLocation LocationDirectory::findVectorManifest(const Superblock &superblock, uint64_t indexId) const {
		const LocationLeafEntry entry = find(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, indexId);
		return {indexId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<VectorManifestLocation> LocationDirectory::findVectorManifestOptional(const Superblock &superblock,
																						uint64_t indexId) const {
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, indexId);
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return VectorManifestLocation{indexId, PageId(entry->segment_id.value()), entry->generation};
	}

	LargeValueManifestLocation LocationDirectory::findLargeValueManifest(const Superblock &superblock,
																		 LargeValueId valueId) const {
		const LocationLeafEntry entry =
				find(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, valueId.value());
		return {valueId, PageId(entry.segment_id.value()), entry.generation};
	}

	std::optional<LargeValueManifestLocation>
	LocationDirectory::findLargeValueManifestOptional(const Superblock &superblock, LargeValueId valueId) const {
		if (!valueId.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_STATE,
										"relocatable LargeValue manifest lookup requires an ID");
		}
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, valueId.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return LargeValueManifestLocation{valueId, PageId(entry->segment_id.value()), entry->generation};
	}

	std::optional<RetireQueueEntry> LocationDirectory::findRetireQueueEntryOptional(const Superblock &superblock,
																					PhysicalSlotId slot) const {
		if (!slot.isValid()) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Retire Queue lookup requires a valid physical slot");
		}
		const std::optional<LocationLeafEntry> entry =
				findOptional(superblock, MetadataDirectoryType::MDT_RETIRE_QUEUE, slot.value());
		if (!entry.has_value()) {
			return std::nullopt;
		}
		return RetireQueueEntry{slot, entry->segment_id, entry->generation,
								static_cast<RetireQueueDisposition>(entry->first_page)};
	}

	void LocationDirectory::visitPages(const Superblock &superblock,
									   const std::function<void(const PageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Page directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_PAGE, [&visitor](const LocationLeafEntry &entry) {
			visitor({PageId(entry.logical_id), entry.segment_id, entry.first_page, entry.generation, entry.free_bytes});
		});
	}

	bool LocationDirectory::visitPagesUntil(const Superblock &superblock,
											const std::function<bool(const PageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Page directory traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_PAGE, [&visitor](const LocationLeafEntry &entry) {
			return visitor(
					{PageId(entry.logical_id), entry.segment_id, entry.first_page, entry.generation, entry.free_bytes});
		});
	}

	void
	LocationDirectory::visitNodePageLocations(const Superblock &superblock,
											  const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Node traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_NODE_ENTITY, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void
	LocationDirectory::visitEdgePageLocations(const Superblock &superblock,
											  const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Edge traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	bool LocationDirectory::visitNodePageLocationsUntil(
			const Superblock &superblock, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Node traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_NODE_ENTITY,
						  [&visitor](const LocationLeafEntry &entry) {
							  return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
											  static_cast<uint32_t>(entry.generation.value())});
						  });
	}

	bool LocationDirectory::visitEdgePageLocationsUntil(
			const Superblock &superblock, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Map Edge traversal requires a visitor");
		}
		return visitUntil(superblock, MetadataDirectoryType::MDT_EDGE_ENTITY,
						  [&visitor](const LocationLeafEntry &entry) {
							  return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
											  static_cast<uint32_t>(entry.generation.value())});
						  });
	}

	bool LocationDirectory::visitNodePageLocationsAfter(const Superblock &superblock, uint64_t afterNodeId,
														size_t maximumMetadataPages,
														const std::function<bool(const EntityPageLocation &)> &visitor,
														size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Entity Map Node traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_NODE_ENTITY, afterNodeId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
									static_cast<uint32_t>(entry.generation.value())});
				},
				visitedMetadataPages);
	}

	bool LocationDirectory::visitEdgePageLocationsAfter(const Superblock &superblock, uint64_t afterEdgeId,
														size_t maximumMetadataPages,
														const std::function<bool(const EntityPageLocation &)> &visitor,
														size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Entity Map Edge traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_EDGE_ENTITY, afterEdgeId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
									static_cast<uint32_t>(entry.generation.value())});
				},
				visitedMetadataPages);
	}

	void
	LocationDirectory::visitEntityProperties(const Superblock &superblock,
											 const std::function<void(const EntityPropertyLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Entity Property directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, [&visitor](const LocationLeafEntry &entry) {
			visitor({EntityPropertyKey::fromDirectoryId(entry.logical_id), PageId(entry.segment_id.value()),
					 entry.first_page, static_cast<uint32_t>(entry.generation.value())});
		});
	}

	bool LocationDirectory::visitEntityPropertiesAfter(
			const Superblock &superblock, uint64_t afterDirectoryId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPropertyLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Entity Property traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_ENTITY_PROPERTY, afterDirectoryId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({EntityPropertyKey::fromDirectoryId(entry.logical_id),
									PageId(entry.segment_id.value()), entry.first_page,
									static_cast<uint32_t>(entry.generation.value())});
				},
				visitedMetadataPages);
	}

	void LocationDirectory::visitStates(const Superblock &superblock,
										const std::function<void(const StateLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable State directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_STATE, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	bool LocationDirectory::visitStatesAfter(const Superblock &superblock, uint64_t afterStateId,
											 size_t maximumMetadataPages,
											 const std::function<bool(const StateLocation &)> &visitor,
											 size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable resumable State traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_STATE, afterStateId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
									static_cast<uint32_t>(entry.generation.value())});
				},
				visitedMetadataPages);
	}

	void LocationDirectory::visitExtents(const Superblock &superblock,
										 const std::function<void(const ExtentLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Extent directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_EXTENT, [&visitor](const LocationLeafEntry &entry) {
			visitor({ExtentId(entry.logical_id), entry.segment_id, entry.first_page, entry.page_count,
					 entry.generation});
		});
	}

	void LocationDirectory::visitIndexRoots(const Superblock &superblock,
											const std::function<void(const IndexRootLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Index Root directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_INDEX_ROOT, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
		});
	}

	void LocationDirectory::visitVectorLocations(const Superblock &superblock,
												 const std::function<void(const VectorLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Vector Location directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_VECTOR_LOCATION, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.first_page,
					 static_cast<uint32_t>(entry.generation.value())});
		});
	}

	void
	LocationDirectory::visitVectorManifests(const Superblock &superblock,
											const std::function<void(const VectorManifestLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Vector Manifest directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, [&visitor](const LocationLeafEntry &entry) {
			visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
		});
	}

	bool LocationDirectory::visitVectorManifestsAfter(
			const Superblock &superblock, uint64_t afterIndexId, size_t maximumMetadataPages,
			const std::function<bool(const VectorManifestLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable Vector Manifest traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_VECTOR_MANIFEST, afterIndexId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor({entry.logical_id, PageId(entry.segment_id.value()), entry.generation});
				},
				visitedMetadataPages);
	}

	void LocationDirectory::visitLargeValueManifests(
			const Superblock &superblock,
			const std::function<void(const LargeValueManifestLocation &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable LargeValue Manifest directory traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, [&visitor](const LocationLeafEntry &entry) {
			visitor({LargeValueId(entry.logical_id), PageId(entry.segment_id.value()), entry.generation});
		});
	}

	bool LocationDirectory::visitLargeValueManifestsAfter(
			const Superblock &superblock, uint64_t afterValueId, size_t maximumMetadataPages,
			const std::function<bool(const LargeValueManifestLocation &)> &visitor,
			size_t *visitedMetadataPages) const {
		if (!visitor || maximumMetadataPages == 0U) {
			throwLocationDirectoryError(
					FormatError::FERR_INVALID_HEADER,
					"relocatable resumable LargeValue Manifest traversal requires a page bound and visitor");
		}
		return visitAfter(
				superblock, MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST, afterValueId, maximumMetadataPages,
				[&visitor](const LocationLeafEntry &entry) {
					return visitor(
							{LargeValueId(entry.logical_id), PageId(entry.segment_id.value()), entry.generation});
				},
				visitedMetadataPages);
	}

	void LocationDirectory::visitRetireQueue(const Superblock &superblock,
											 const std::function<void(const RetireQueueEntry &)> &visitor) const {
		if (!visitor) {
			throwLocationDirectoryError(FormatError::FERR_INVALID_HEADER,
										"relocatable Retire Queue traversal requires a visitor");
		}
		visit(superblock, MetadataDirectoryType::MDT_RETIRE_QUEUE, [&visitor](const LocationLeafEntry &entry) {
			visitor({PhysicalSlotId(entry.logical_id), entry.segment_id, entry.generation,
					 static_cast<RetireQueueDisposition>(entry.first_page)});
		});
	}

} // namespace graph::storage
