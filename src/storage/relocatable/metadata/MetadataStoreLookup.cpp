#include "MetadataStoreOperationsInternal.hpp"

namespace graph::storage {

	CatalogEntry RelocatableMetadataStore::findCatalog(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata Catalog lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return catalog_.find(snapshot.superblock(), segmentId);
	}

	std::optional<RetireQueueEntry>
	RelocatableMetadataStore::findRetireQueueEntryOptional(const MetadataSnapshotPin &snapshot,
														   PhysicalSlotId slot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Retire Queue lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findRetireQueueEntryOptional(snapshot.superblock(), slot);
	}

	void RelocatableMetadataStore::visitRetireQueueEntries(
			const MetadataSnapshotPin &snapshot, const std::function<void(const RetireQueueEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Retire Queue traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitRetireQueue(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitCatalogEntries(const MetadataSnapshotPin &snapshot,
													   const std::function<void(const CatalogEntry &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Catalog traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		catalog_.visit(snapshot.superblock(), visitor);
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentIndexPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable index page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_INDEX_PAGE_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentEntityPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_ENTITY_PAGE_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorAdjacencyPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_ADJACENCY_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorRawPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable raw Vector page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_RAW_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findRetiredVectorRawPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable retired Raw Vector page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_RAW_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_RETIRED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentVectorPqPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable PQ page-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_VECTOR_PQ_POOL) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentLargeValueDataPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared LargeValue data-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL) {
				return;
			}
			if (entry.owner_id != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue data pool is outside its shared namespace");
			}
			const uint8_t eligible =
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentLargeValueManifestPagePools(const MetadataSnapshotPin &snapshot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable shared LargeValue Manifest-pool scan requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
				return;
			}
			if (entry.owner_id != RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue Manifest pool is outside its shared namespace");
			}
			const uint8_t eligible =
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry>
	RelocatableMetadataStore::findCurrentPropertyPagePools(const MetadataSnapshotPin &snapshot,
														   uint64_t poolOwnerId) const {
        debug::PerfTrace::addValue("native_graph.write.property_pool_catalog_scans", 1);
		if (!snapshots_.owns(snapshot) || poolOwnerId == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Property page-pool scan requires this store's active snapshot and owner");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools, poolOwnerId](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) &
					static_cast<uint8_t>(entry.owner_id == poolOwnerId) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	std::vector<CatalogEntry> RelocatableMetadataStore::findCurrentStatePagePools(const MetadataSnapshotPin &snapshot,
																				  uint64_t poolOwnerId) const {
		if (!snapshots_.owns(snapshot) || poolOwnerId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State page-pool scan requires this store's active snapshot and owner");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<CatalogEntry> pools;
		catalog_.visit(snapshot.superblock(), [this, &snapshot, &pools, poolOwnerId](const CatalogEntry &entry) {
			const uint8_t eligible =
					static_cast<uint8_t>(entry.kind == SegmentKind::SGK_STATE_PAGE_POOL) &
					static_cast<uint8_t>(entry.owner_id == poolOwnerId) &
					static_cast<uint8_t>(entry.state == SegmentState::SGS_SEALED) &
					static_cast<uint8_t>(allocator_.isAllocated(snapshot.superblock(), entry.physical_slot));
			if (eligible != 0U) {
				pools.push_back(entry);
			}
		});
		std::ranges::sort(pools, {}, &CatalogEntry::segment_id);
		return pools;
	}

	PageLocation RelocatableMetadataStore::findPageLocation(const MetadataSnapshotPin &snapshot, PageId pageId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Page Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findPage(snapshot.superblock(), pageId);
	}

	std::optional<PageLocation> RelocatableMetadataStore::findPageLocationOptional(const MetadataSnapshotPin &snapshot,
																				   PageId pageId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Page Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findPageOptional(snapshot.superblock(), pageId);
	}

	ExtentLocation RelocatableMetadataStore::findExtentLocation(const MetadataSnapshotPin &snapshot,
																ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Extent Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findExtent(snapshot.superblock(), extentId);
	}

	std::optional<ExtentLocation>
	RelocatableMetadataStore::findExtentLocationOptional(const MetadataSnapshotPin &snapshot, ExtentId extentId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Extent Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findExtentOptional(snapshot.superblock(), extentId);
	}

	EntityPageLocation RelocatableMetadataStore::findNodePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Node lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findNodePageLocation(snapshot.superblock(), nodeId);
	}

	EntityPageLocation RelocatableMetadataStore::findEdgePageLocation(const MetadataSnapshotPin &snapshot,
																	  uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Map Edge lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEdgePageLocation(snapshot.superblock(), edgeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findNodePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Node lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findNodePageLocationOptional(snapshot.superblock(), nodeId);
	}

	std::optional<EntityPageLocation>
	RelocatableMetadataStore::findEdgePageLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Entity Map Edge lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEdgePageLocationOptional(snapshot.superblock(), edgeId);
	}

	void RelocatableMetadataStore::findNodePageLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> nodeIds,
			const std::span<std::optional<EntityPageLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable Node multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPageLocationsSorted(snapshot.superblock(), MetadataDirectoryType::MDT_NODE_ENTITY,
																			 nodeIds, results, cancellation);
	}

	void RelocatableMetadataStore::findEdgePageLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const uint64_t> edgeIds,
			const std::span<std::optional<EntityPageLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable Edge multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPageLocationsSorted(snapshot.superblock(), MetadataDirectoryType::MDT_EDGE_ENTITY,
																			 edgeIds, results, cancellation);
	}

	void RelocatableMetadataStore::visitNodePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitNodePageLocations(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitEdgePageLocations(
			const MetadataSnapshotPin &snapshot, const std::function<void(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitEdgePageLocations(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitNodePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Node traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitNodePageLocationsUntil(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitEdgePageLocationsUntil(
			const MetadataSnapshotPin &snapshot, const std::function<bool(const EntityPageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Entity Map Edge traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitEdgePageLocationsUntil(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitNodePageLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterNodeId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPageLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable Entity Map Node traversal "
								  "requires this store's active snapshot, page bound, and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitNodePageLocationsAfter(snapshot.superblock(), afterNodeId, maximumMetadataPages, visitor,
													  visitedMetadataPages);
	}

	bool RelocatableMetadataStore::visitEdgePageLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterEdgeId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPageLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable Entity Map Edge traversal "
								  "requires this store's active snapshot, page bound, and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitEdgePageLocationsAfter(snapshot.superblock(), afterEdgeId, maximumMetadataPages, visitor,
													  visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitExtentLocations(const MetadataSnapshotPin &snapshot,
												   const std::function<void(const ExtentLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Extent directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitExtents(snapshot.superblock(), visitor);
	}

	bool
	RelocatableMetadataStore::visitPageLocationsUntil(const MetadataSnapshotPin &snapshot,
													  const std::function<bool(const PageLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Page directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitPagesUntil(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitEntityPropertyLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const EntityPropertyLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Property directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitEntityProperties(snapshot.superblock(), visitor);
	}

	LocationDirectoryRangeCursor RelocatableMetadataStore::openEntityPropertyLocationRangeCursor(
			const MetadataSnapshotPin &snapshot, const uint64_t lowerDirectoryId, const uint64_t upperDirectoryId,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable Entity Property range cursor requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.openRangeCursor(snapshot.superblock(), MetadataDirectoryType::MDT_ENTITY_PROPERTY,
									  lowerDirectoryId, upperDirectoryId, cancellation);
	}

	bool RelocatableMetadataStore::visitEntityPropertyLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterDirectoryId, size_t maximumMetadataPages,
			const std::function<bool(const EntityPropertyLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable resumable Entity Property traversal "
																   "requires this store's active snapshot, page bound, "
																   "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitEntityPropertiesAfter(snapshot.superblock(), afterDirectoryId, maximumMetadataPages,
													 visitor, visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitStateLocations(const MetadataSnapshotPin &snapshot,
												  const std::function<void(const StateLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable State directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitStates(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitStateLocationsAfter(const MetadataSnapshotPin &snapshot, uint64_t afterStateId,
															size_t maximumMetadataPages,
															const std::function<bool(const StateLocation &)> &visitor,
															size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable State traversal requires this store's active snapshot, page "
								  "bound, and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitStatesAfter(snapshot.superblock(), afterStateId, maximumMetadataPages, visitor,
										   visitedMetadataPages);
	}

	void
	RelocatableMetadataStore::visitIndexRoots(const MetadataSnapshotPin &snapshot,
											  const std::function<void(const IndexRootLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Index Root directory traversal requires this store's active snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitIndexRoots(snapshot.superblock(), visitor);
	}

	void
	RelocatableMetadataStore::visitVectorLocations(const MetadataSnapshotPin &snapshot,
												   const std::function<void(const VectorLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Location Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitVectorLocations(snapshot.superblock(), visitor);
	}

	void RelocatableMetadataStore::visitVectorManifestLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const VectorManifestLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitVectorManifests(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitVectorManifestLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterIndexId, size_t maximumMetadataPages,
			const std::function<bool(const VectorManifestLocation &)> &visitor, size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable resumable Vector Manifest traversal "
																   "requires this store's active snapshot, page bound, "
																   "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitVectorManifestsAfter(snapshot.superblock(), afterIndexId, maximumMetadataPages, visitor,
													visitedMetadataPages);
	}

	void RelocatableMetadataStore::visitLargeValueManifestLocations(
			const MetadataSnapshotPin &snapshot,
			const std::function<void(const LargeValueManifestLocation &)> &visitor) const {
		if (!snapshots_.owns(snapshot) || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable LargeValue Manifest Directory traversal requires this store's active "
								  "snapshot and a visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.visitLargeValueManifests(snapshot.superblock(), visitor);
	}

	bool RelocatableMetadataStore::visitLargeValueManifestLocationsAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterValueId, size_t maximumMetadataPages,
			const std::function<bool(const LargeValueManifestLocation &)> &visitor,
			size_t *visitedMetadataPages) const {
		if (!snapshots_.owns(snapshot) || !visitor || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable resumable LargeValue Manifest traversal requires this store's active "
								  "snapshot, page bound, "
								  "and visitor");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.visitLargeValueManifestsAfter(snapshot.superblock(), afterValueId, maximumMetadataPages,
														visitor, visitedMetadataPages);
	}

	EntityPropertyLocation RelocatableMetadataStore::findEntityPropertyLocation(const MetadataSnapshotPin &snapshot,
																				const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity Property Directory lookup requires a valid key and snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEntityProperty(snapshot.superblock(), key);
	}

	std::optional<EntityPropertyLocation>
	RelocatableMetadataStore::findEntityPropertyLocationOptional(const MetadataSnapshotPin &snapshot,
																 const EntityPropertyKey &key) const {
		if (!snapshots_.owns(snapshot) || !key.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable optional Entity Property lookup requires a valid key and snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findEntityPropertyOptional(snapshot.superblock(), key);
	}

	void RelocatableMetadataStore::findEntityPropertyLocationsSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<std::optional<EntityPropertyLocation>> results,
			const CancellationCheckpoint *cancellation) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable Entity Property multi-lookup requires this store's active snapshot");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		locations_.findEntityPropertiesSorted(snapshot.superblock(), keys, results, cancellation);
	}

	StateLocation RelocatableMetadataStore::findStateLocation(const MetadataSnapshotPin &snapshot,
															  uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable State Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findState(snapshot.superblock(), stateId);
	}

	std::optional<StateLocation>
	RelocatableMetadataStore::findStateLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t stateId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional State Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findStateOptional(snapshot.superblock(), stateId);
	}

	VectorLocation RelocatableMetadataStore::findVectorLocation(const MetadataSnapshotPin &snapshot,
																uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Location Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorLocation(snapshot.superblock(), vectorId);
	}

	std::optional<VectorLocation>
	RelocatableMetadataStore::findVectorLocationOptional(const MetadataSnapshotPin &snapshot, uint64_t vectorId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Location Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorLocationOptional(snapshot.superblock(), vectorId);
	}

	VectorManifestLocation RelocatableMetadataStore::findVectorManifest(const MetadataSnapshotPin &snapshot,
																		uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorManifest(snapshot.superblock(), indexId);
	}

	std::optional<VectorManifestLocation>
	RelocatableMetadataStore::findVectorManifestOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Vector Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findVectorManifestOptional(snapshot.superblock(), indexId);
	}

	LargeValueManifestLocation RelocatableMetadataStore::findLargeValueManifest(const MetadataSnapshotPin &snapshot,
																				LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable LargeValue Manifest Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findLargeValueManifest(snapshot.superblock(), valueId);
	}

	std::optional<LargeValueManifestLocation>
	RelocatableMetadataStore::findLargeValueManifestOptional(const MetadataSnapshotPin &snapshot,
															 LargeValueId valueId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable optional LargeValue Manifest Directory "
																   "lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findLargeValueManifestOptional(snapshot.superblock(), valueId);
	}

	IndexRootLocation RelocatableMetadataStore::findIndexRoot(const MetadataSnapshotPin &snapshot,
															  uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Index Root Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findIndexRoot(snapshot.superblock(), indexId);
	}

	std::optional<IndexRootLocation>
	RelocatableMetadataStore::findIndexRootOptional(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable optional Index Root Directory lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return locations_.findIndexRootOptional(snapshot.superblock(), indexId);
	}

	std::vector<PageLocation> RelocatableMetadataStore::findPageLocationsForSegment(const MetadataSnapshotPin &snapshot,
																					SegmentId segmentId) const {
		if (!snapshots_.owns(snapshot) || !segmentId.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Page directory scan requires this store's active snapshot and a Segment ID");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		std::vector<PageLocation> locations;
		locations_.visitPages(snapshot.superblock(), [&](const PageLocation &location) {
			if (location.segment_id == segmentId) {
				locations.push_back(location);
			}
		});
		return locations;
	}

	bool RelocatableMetadataStore::isPhysicalSlotAllocated(const MetadataSnapshotPin &snapshot,
														   PhysicalSlotId slot) const {
		if (!snapshots_.owns(snapshot)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata free-space lookup requires this store's active snapshot pin");
		}
		MetadataPageReadContext::Scope readScope(snapshot.readContext(), *pages_.storageIO());
		return allocator_.isAllocated(snapshot.superblock(), slot);
	}

	RelocatableTailTruncationResult RelocatableMetadataStore::truncateReleasedTail() {
        // This operation changes no logical directory or field. Its existing
        // census protects every allocated Catalog/retirement/metadata pool and
        // refuses truncation while any source snapshot remains pinned.
		MetadataWriteGuard writer = beginWrite(true);
		const uint64_t originalBytes = io_->fileSize();
		if (snapshots_.activeSnapshotCount() != 0U) {
			return {RelocatableTailTruncationStatus::VTTS_AWAITING_SNAPSHOTS, originalBytes, originalBytes, 0};
		}
		const Superblock current = bootstrap_.recover();
        MetadataPageReadContext::Scope tailReads(snapshots_.createWriterReadContext(writer, current), *io_);
		if (originalBytes < current.data_region_offset) {
			throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "relocatable tail truncation found a data file shorter than its bootstrap region");
		}
		const uint64_t dataBytes = originalBytes - current.data_region_offset;
		const uint64_t physicalSlots = dataBytes / RSS_SEGMENT_SIZE;
		const auto validateAllocatedSlot = [&](PhysicalSlotId slot, const char *message) {
			const uint8_t invalid = static_cast<uint8_t>(!slot.isValid()) |
									static_cast<uint8_t>(slot.value() > physicalSlots) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE, message);
			}
		};
		catalog_.visit(current, [&](const CatalogEntry &entry) {
			if (entry.state == SegmentState::SGS_SEALED || entry.state == SegmentState::SGS_RETIRED) {
				validateAllocatedSlot(entry.physical_slot,
									  "relocatable tail truncation found a current Catalog slot outside the data file");
			}
		});
		locations_.visitRetireQueue(current, [&](const RetireQueueEntry &entry) {
			validateAllocatedSlot(entry.physical_slot,
								  "relocatable tail truncation found a queued retired slot outside the data file");
		});
		for (const PhysicalSlotId poolSlot: metadata_page_pool_.poolSlots()) {
			validateAllocatedSlot(poolSlot,
								  "relocatable tail truncation found a metadata page pool outside the data file");
		}

		uint64_t retainedSlots = physicalSlots;
		while (retainedSlots != 0U && !allocator_.isAllocated(current, PhysicalSlotId(retainedSlots))) {
			--retainedSlots;
		}
		const uint64_t retainedBytes = current.data_region_offset + retainedSlots * RSS_SEGMENT_SIZE;
		if (retainedBytes >= originalBytes) {
			return {RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE, originalBytes, originalBytes, 0};
		}
		const file_handle_t nativeHandle = io_->getWriteFd();
		if (nativeHandle == INVALID_FILE_HANDLE) {
			return {RelocatableTailTruncationStatus::VTTS_NATIVE_IO_REQUIRED, originalBytes, originalBytes, 0};
		}
		try {
			io_->flushStream();
			// A failed pre-truncate fence guarantees the operating system has not
			// shortened the tail, so callers can safely retry unchanged.
			io_->sync();
			// Keep Buffer Pool tail invalidation and native truncate fault handling in
			// the common StorageIO boundary before the final durability fence.
			io_->truncate(retainedBytes);
		} catch (const std::runtime_error &) {
			return {RelocatableTailTruncationStatus::VTTS_IO_ERROR, originalBytes, originalBytes, 0};
		}
		try {
			io_->sync();
		} catch (const std::runtime_error &) {
			// The truncate already succeeded locally, but its durability is unknown.
			// It is still safe to reopen because this was an already-free suffix.
			return {RelocatableTailTruncationStatus::VTTS_IO_ERROR, originalBytes, retainedBytes, 0};
		}
		return {RelocatableTailTruncationStatus::VTTS_TRUNCATED, originalBytes, retainedBytes,
				physicalSlots - retainedSlots};
	}

	RelocatableCompactionObservation RelocatableMetadataStore::observeCompactionState() const {
		MetadataSnapshotPin snapshot = openSnapshot();
		const auto pressure = snapshots_.snapshotPressure(&snapshot);
		RelocatableCompactionObservation observation;
		observation.generation = snapshot.superblock().generation;
		observation.checkpoint_lsn = snapshot.superblock().checkpoint_lsn;
		observation.active_snapshot_count = pressure.active_snapshot_count;
		observation.oldest_snapshot_generation = pressure.oldest_generation;
		if (pressure.oldest_generation.isValid() && pressure.oldest_generation < observation.generation)
			observation.snapshot_generation_lag = observation.generation.value() - pressure.oldest_generation.value();
		catalog_.visit(snapshot.superblock(), [&observation](const CatalogEntry &entry) {
			observation.sealed_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_SEALED);
			observation.retired_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_RETIRED);
			observation.obsolete_segments += static_cast<uint64_t>(entry.state == SegmentState::SGS_OBSOLETE);
		});
		locations_.visitRetireQueue(snapshot.superblock(), [&observation](const RetireQueueEntry &entry) {
			++observation.retire_queue_entries;
			if (!observation.oldest_retired_generation.isValid() || entry.retired_generation < observation.oldest_retired_generation)
				observation.oldest_retired_generation = entry.retired_generation;
			observation.pending_relocation_sources +=
					static_cast<uint64_t>(entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE);
		});
		snapshot.release();
		return observation;
	}

	MetadataPagePoolStats RelocatableMetadataStore::pagePoolStats() const { return metadata_page_pool_.stats(); }
	uint64_t RelocatableMetadataStore::dataMutationEpoch() const { return io_->dataMutationEpoch(); }

	Superblock RelocatableMetadataStore::migratePageDirectoryToBootstrap(LogSequenceNumber checkpointLsn,
																		 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot migrate the Page Directory");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.usesPageDirectoryBootstrap()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Page Directory bootstrap migration was already published");
		}
		MetadataPageRouteDirectory routes(pages_);
		const size_t requiredFrames = routes.initialBootstrapFrameCount(current);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.reserveMetadataFrames(requiredFrames);
		static_cast<void>(routes.prepareInitialBootstrapMigration(batch));
		batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
        pages_.rebindSuperblock(batch.candidate());
        // Migration already validates the complete graph. Establish retirement
        // coverage here, so the first ordinary publication stays incremental.
        snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	std::unique_ptr<RelocatableMetadataStore>
	RelocatableMetadataStore::initializeNewComplete(std::shared_ptr<StorageIO> io, const Superblock &initial, std::shared_ptr<std::pmr::memory_resource> memoryOwner, std::shared_ptr<std::pmr::memory_resource> controlOwner, std::shared_ptr<std::pmr::memory_resource> leaseMemoryOwner) {
        std::optional<utils::OwnedAllocationScope> allocation;
        if (memoryOwner) allocation.emplace(memoryOwner);
		if (!initial.checkpoint_lsn.isValid() ||
			initial.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable complete initialization requires a non-terminal checkpoint LSN");
		}
		auto metadata = initializeNew(std::move(io), initial, std::move(memoryOwner), std::move(controlOwner), std::move(leaseMemoryOwner));
		static_cast<void>(metadata->convergeInitialBootstrap(LogSequenceNumber(initial.checkpoint_lsn.value() + 1U)));
		return metadata;
	}

	Superblock RelocatableMetadataStore::convergeInitialBootstrap(LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot converge initial bootstrap");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.usesPageDirectoryBootstrap() || current.catalog_root.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable initial bootstrap convergence requires an empty pre-bootstrap Catalog");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		// A capacity preflight may create an empty metadata pool. Reconcile that
		// pool into the candidate Catalog before calculating the final route set;
		// otherwise a newly created file can expose route frames without durable
		// pool ownership after the first later metadata COW.
		pool_ownership_.preparePublication(batch);
		MetadataPageRouteDirectory routes(pages_);
		for (size_t pass = 0U; pass < 8U; ++pass) {
			const size_t requiredFrames = routes.initialBootstrapFrameCount(batch);
			if (batch.ensureMetadataFrameCapacity(requiredFrames) == 0U) {
				break;
			}
			pool_ownership_.preparePublication(batch);
			if (pass == 7U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable initial bootstrap route capacity did not converge");
			}
		}
		batch.reserveMetadataFrames(routes.initialBootstrapFrameCount(batch));
		static_cast<void>(routes.prepareInitialBootstrapMigration(batch));
		batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
		pages_.rebindSuperblock(batch.candidate());
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return batch.candidate();
	}

	uint64_t RelocatableMetadataStore::activeSnapshotCount() const { return snapshots_.activeSnapshotCount(); }


} // namespace graph::storage
