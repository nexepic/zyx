/**
 * @file VectorManifestStore.cpp
 * @brief Atomic materialization and snapshot reads for immutable vector manifests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"

#include <array>

#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

namespace graph::storage {

	VectorManifestStore::VectorManifestStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata) :
		VectorManifestStore(std::move(io), metadata, SharedPagePoolOpenMode::SPPOM_RECONCILE) {}

	VectorManifestStore::VectorManifestStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
			SharedPagePoolOpenMode openMode) :
		metadata_(metadata), allocator_(io, metadata, openMode), extents_(std::move(io), metadata) {}

	void VectorManifestStore::validateReferencedCodebook(const VectorManifest &manifest) {
		if (manifest.codebook_id == 0U) {
			return;
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			VectorCodebookStore codebooks(extents_, metadata_);
			const VectorCodebookManifest codebook = codebooks.manifest(snapshot, manifest.codebook_id);
			const uint8_t hasTooManyCentroids =
					static_cast<uint8_t>(codebook.header.centroid_count > 256U); // ZYX_COV_EXCL_LINE:
			// VectorCodebookManifestCodec rejects counts above the one-byte PQ code domain before publication.
			const uint8_t invalid = static_cast<uint8_t>(codebook.header.codebook_id != manifest.codebook_id) |
									static_cast<uint8_t>(codebook.header.generation != manifest.codebook_generation) |
									static_cast<uint8_t>(codebook.header.dimensions != manifest.dimensions) |
									static_cast<uint8_t>(codebook.header.element_type !=
																			VectorCodebookElementType::VCET_FLOAT32) |
									static_cast<uint8_t>(codebook.header.element_bytes != sizeof(float)) |
									static_cast<uint8_t>(codebook.header.centroid_count == 0U) | hasTooManyCentroids;
			if (invalid == 0U) {
				const uint64_t maximumPayloadBytes =
						static_cast<uint64_t>(manifest.dimensions) * 256U * sizeof(float);
				codebooks.validate(snapshot, manifest.codebook_id, maximumPayloadBytes,
								   VectorCodebookStore::requiredDataExtentCount(codebook.header.payload_bytes));
			}
			snapshot.release();
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable vector manifest references an unavailable or mismatched codebook extent");
			}
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void VectorManifestStore::validateReferencedPqMapping(const VectorManifest &manifest) const {
		if (manifest.pq_mapping_index_id == 0U) {
			return;
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const std::optional<IndexRootLocation> root =
					metadata_.findIndexRootOptional(snapshot, manifest.pq_mapping_index_id);
			const uint8_t invalid =
					static_cast<uint8_t>(!root.has_value()) |
					static_cast<uint8_t>(root.has_value() && root->root_page_id != manifest.pq_mapping_root_page_id) |
					static_cast<uint8_t>(root.has_value() &&
										 root->root_generation != manifest.pq_mapping_root_generation);
			snapshot.release();
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable vector manifest references an unavailable or mismatched PQ mapping root");
			}
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void VectorManifestStore::validateReferencedVectorNodeMapping(const VectorManifest &manifest) const {
		if (manifest.vector_node_mapping_index_id == 0U) {
			return;
		}
		VectorNodeMapping::validateMappingIndexId(manifest.vector_node_mapping_index_id);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const std::optional<IndexRootLocation> root =
					metadata_.findIndexRootOptional(snapshot, manifest.vector_node_mapping_index_id);
			const uint8_t invalid =
					static_cast<uint8_t>(!root.has_value()) |
					static_cast<uint8_t>(root.has_value() &&
									 root->root_page_id != manifest.vector_node_mapping_root_page_id) |
					static_cast<uint8_t>(root.has_value() &&
									 root->root_generation != manifest.vector_node_mapping_root_generation);
			snapshot.release();
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable vector manifest references an unavailable or mismatched Vector-to-Node mapping root");
			}
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	void VectorManifestStore::validateReferencedAdjacencyMapping(const VectorManifest &manifest) const {
		if (manifest.adjacency_mapping_index_id == 0U) {
			return;
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const std::optional<IndexRootLocation> root =
					metadata_.findIndexRootOptional(snapshot, manifest.adjacency_mapping_index_id);
			const uint8_t invalid =
					static_cast<uint8_t>(!root.has_value()) |
					static_cast<uint8_t>(root.has_value() &&
										 root->root_page_id != manifest.adjacency_mapping_root_page_id) |
					static_cast<uint8_t>(root.has_value() &&
										 root->root_generation != manifest.adjacency_mapping_root_generation);
			snapshot.release();
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable vector manifest references an unavailable or mismatched adjacency mapping root");
			}
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	std::optional<SharedPagePoolAllocation>
	VectorManifestStore::allocate(SharedPagePoolAllocationRequest request, const VectorManifestPage &page,
								  PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
								  const MetadataPublishOptions &options) {
		const VectorManifest &manifest = page.manifest();
		const uint8_t invalid = static_cast<uint8_t>(request.pool_kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(request.page_id != manifest.page_id) |
								static_cast<uint8_t>(request.pool_owner_id != manifest.index_id) |
								static_cast<uint8_t>(request.page_owner_id != manifest.index_id) |
								static_cast<uint8_t>(request.page_owner_kind != PageOwnerKind::POK_VECTOR_MANIFEST) |
								static_cast<uint8_t>(request.generation != manifest.generation) |
								static_cast<uint8_t>(manifest.committed_lsn != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable vector manifest allocation does not match its durable identity");
		}
		validateReferencedCodebook(manifest);
		validateReferencedVectorNodeMapping(manifest);
		validateReferencedPqMapping(manifest);
		validateReferencedAdjacencyMapping(manifest);
		const std::vector<std::byte> payload = page.serialize();
		CatalogSlotReservationRequest catalogRequest;
		catalogRequest.segment_id = request.segment_id;
		catalogRequest.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		catalogRequest.owner_id = manifest.index_id;
		catalogRequest.segment_generation = manifest.generation;
		catalogRequest.state = SegmentState::SGS_SEALED;
		catalogRequest.require_absent = true;
		const PageLocation location{request.page_id, request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
									request.generation, 0};
		const PageOwnerHeader owner{request.page_id,
									manifest.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0,
									static_cast<uint32_t>(payload.size()),
									request.generation,
									checkpointLsn};
		const VectorManifestLocation manifestLocation{manifest.index_id, manifest.page_id, manifest.generation};
		const CatalogSlotReservationPreparer preparer = [this, owner, payload](std::span<const CatalogEntry> entries) {
			allocator_.writePreparedPool(entries.front(), owner, payload);
		};
		const std::optional<CatalogSlotReservations> reserved =
				metadata_.reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&catalogRequest, 1), std::span(&location, 1), std::span(&manifestLocation, 1),
						firstSlot, lastSlot, checkpointLsn, preparer, options);
		if (!reserved.has_value()) {
			return std::nullopt;
		}
		return SharedPagePoolAllocation{reserved->superblock, reserved->entries.front(), location, owner};
	}

	VectorManifestMaterializationResult
	VectorManifestStore::materialize(VectorManifest manifest, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
									 LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		const uint8_t invalid =
				static_cast<uint8_t>(manifest.index_id == 0U) | static_cast<uint8_t>(manifest.page_id.isValid()) |
				static_cast<uint8_t>(manifest.generation.isValid()) |
				static_cast<uint8_t>(manifest.committed_lsn.isValid()) | static_cast<uint8_t>(!firstSlot.isValid()) |
				static_cast<uint8_t>(!lastSlot.isValid()) | static_cast<uint8_t>(firstSlot > lastSlot) |
				static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Manifest materialization requires unassigned identity, placement, and "
								  "checkpoint LSN");
		}
		validateReferencedCodebook(manifest);
		validateReferencedVectorNodeMapping(manifest);
		validateReferencedPqMapping(manifest);
		validateReferencedAdjacencyMapping(manifest);
		VectorManifestMaterializationResult result;
		const NewVectorManifestPagePreparer preparer = [this, &manifest, &result,
														checkpointLsn](const CatalogEntry &pool, PageId pageId,
																	   PageLocation &location) {
			manifest.page_id = pageId;
			manifest.generation = pool.generation;
			manifest.committed_lsn = checkpointLsn;
			const PageOwnerHeader owner{pageId,
										manifest.index_id,
										PageOwnerKind::POK_VECTOR_MANIFEST,
										0,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										pool.generation,
										checkpointLsn};
			const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
			const SharedPagePoolPreparedAppend prepared{owner, page.serialize(), 0};
			allocator_.prepareNewPoolPages(pool, std::span(&prepared, 1), std::span(&location, 1), checkpointLsn);
			result.manifest = manifest;
			result.allocation.pool = pool;
			result.allocation.location = location;
			result.allocation.owner = owner;
		};
		result.allocation.superblock = metadata_.materializeNewVectorManifestPage(
				manifest.index_id, firstSlot, lastSlot, checkpointLsn, preparer, options);
		return result;
	}

	SharedPagePoolAllocation VectorManifestStore::replace(const VectorManifestPage &page, SegmentId segmentId,
														  LogSequenceNumber expectedOwnerLsn,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options) {
		const VectorManifest &manifest = page.manifest();
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const VectorManifestLocation location = metadata_.findVectorManifest(snapshot, manifest.index_id);
		const uint8_t invalid = static_cast<uint8_t>(location.manifest_page_id != manifest.page_id) |
								static_cast<uint8_t>(location.generation != manifest.generation) |
								static_cast<uint8_t>(manifest.committed_lsn != checkpointLsn);
		snapshot.release();
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable vector manifest replacement does not match its durable directory identity");
		}
		validateReferencedCodebook(manifest);
		validateReferencedPqMapping(manifest);
		validateReferencedAdjacencyMapping(manifest);
		const std::vector<std::byte> payload = page.serialize();
		SharedPagePoolReplacementRequest request;
		request.segment_id = segmentId;
		request.generation = manifest.generation;
		request.page_id = manifest.page_id;
		request.expected_owner_lsn = expectedOwnerLsn;
		request.page_owner_id = manifest.index_id;
		request.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		request.page_free_bytes = 0;
		request.payload = payload;
		return allocator_.replacePage(request, checkpointLsn, options);
	}

	bool VectorManifestStore::hasReplacementFrameCapacity(const CatalogEntry &pool, size_t requiredFrames) const {
		return pool.kind == SegmentKind::SGK_VECTOR_MANIFEST_POOL && pool.state == SegmentState::SGS_SEALED &&
			   requiredFrames != 0U &&
			   requiredFrames <= allocator_.availableFrameCount(pool);
	}

	void VectorManifestStore::prepareReplacement(const CatalogEntry &pool, const PageLocation &expected,
												 LogSequenceNumber expectedOwnerLsn, const VectorManifestPage &page,
												 LogSequenceNumber checkpointLsn, PageLocation &replacement) const {
		const VectorManifest &manifest = page.manifest();
		const std::vector<std::byte> payload = page.serialize();
		const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
								static_cast<uint8_t>(expected.generation != pool.generation) |
								static_cast<uint8_t>(manifest.index_id != pool.owner_id) |
								static_cast<uint8_t>(manifest.page_id != expected.page_id) |
								static_cast<uint8_t>(manifest.generation != pool.generation) |
								static_cast<uint8_t>(manifest.committed_lsn != checkpointLsn) |
								static_cast<uint8_t>(!expectedOwnerLsn.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest COW preparation has an invalid pool, source, or page identity");
		}
		const PageOwnerHeader owner{manifest.page_id,
									manifest.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0U,
									static_cast<uint32_t>(payload.size()),
									pool.generation,
									checkpointLsn};
		const SharedPagePoolPreparedReplacement prepared{owner, expectedOwnerLsn, payload, 0U};
		const std::array<PageLocation, 1> sources{{expected}};
		const std::array<SharedPagePoolPreparedReplacement, 1> replacements{{prepared}};
		allocator_.prepareMutationPages(pool, sources, replacements, {}, std::span(&replacement, 1), {}, checkpointLsn);
	}

	void VectorManifestStore::prepareNewPoolReplacement(const CatalogEntry &pool, const VectorManifestPage &page,
														LogSequenceNumber checkpointLsn,
														PageLocation &replacement) const {
		const VectorManifest &manifest = page.manifest();
		const std::vector<std::byte> payload = page.serialize();
		const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(pool.owner_id != manifest.index_id) |
								static_cast<uint8_t>(!pool.segment_id.isValid()) |
								static_cast<uint8_t>(!pool.physical_slot.isValid()) |
								static_cast<uint8_t>(pool.generation != Generation(1U)) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(pool.committed_lsn != checkpointLsn) |
								static_cast<uint8_t>(!replacement.page_id.isValid()) |
								static_cast<uint8_t>(replacement.segment_id != pool.segment_id) |
								static_cast<uint8_t>(replacement.generation != pool.generation) |
								static_cast<uint8_t>(replacement.page_index != 0U) |
								static_cast<uint8_t>(replacement.free_bytes != 0U) |
								static_cast<uint8_t>(manifest.page_id != replacement.page_id) |
								static_cast<uint8_t>(manifest.generation != pool.generation) |
								static_cast<uint8_t>(manifest.committed_lsn != checkpointLsn) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest new-pool preparation has an invalid pool or replacement identity");
		}
		const PageOwnerHeader owner{manifest.page_id,
									manifest.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0U,
									static_cast<uint32_t>(payload.size()),
									pool.generation,
									checkpointLsn};
		const SharedPagePoolPreparedAppend prepared{owner, payload, 0U};
		const std::array<SharedPagePoolPreparedAppend, 1> pages{{prepared}};
		allocator_.prepareNewPoolPages(pool, pages, std::span(&replacement, 1), checkpointLsn);
	}

	void VectorManifestStore::prepareNewPoolInitial(const CatalogEntry &pool, const VectorManifestPage &page,
													LogSequenceNumber checkpointLsn, PageLocation &initial) const {
		prepareNewPoolReplacement(pool, page, checkpointLsn, initial);
	}

	void VectorManifestStore::prepareReplacementSourceRetirement(const CatalogEntry &pool, const PageLocation &expected,
																 LogSequenceNumber expectedOwnerLsn,
																 LogSequenceNumber checkpointLsn) const {
		const uint8_t invalid = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
								static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
								static_cast<uint8_t>(expected.generation != pool.generation) |
								static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
								static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
								static_cast<uint8_t>(!expectedOwnerLsn.isValid()) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest source retirement has an invalid pool, page, or owner fence");
		}
		const SharedPagePoolPreparedRetirement retirement{expected, pool.owner_id, PageOwnerKind::POK_VECTOR_MANIFEST,
														  expectedOwnerLsn};
		allocator_.prepareMutationPages(pool, {}, {}, {}, {}, {}, checkpointLsn, std::span(&retirement, 1));
	}

	Superblock VectorManifestStore::retire(uint64_t indexId, LogSequenceNumber expectedOwnerLsn,
										   LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		if (indexId == 0U || !expectedOwnerLsn.isValid() || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable vector manifest retirement requires an index ID and owner/checkpoint LSNs");
		}
		MetadataSnapshotPin placementSnapshot = metadata_.openSnapshot();
		const VectorManifestLocation placement = metadata_.findVectorManifest(placementSnapshot, indexId);
		const PageLocation placementLocation =
				metadata_.findPageLocation(placementSnapshot, placement.manifest_page_id);
		placementSnapshot.release();
		// A failed pre-header publication can leave a current owner marked retired;
		// recovery reactivates it before this retry validates the live owner LSN.
		allocator_.reconcilePool(placementLocation.segment_id);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const VectorManifestLocation manifestLocation = metadata_.findVectorManifest(snapshot, indexId);
		const SharedPagePoolContents contents = allocator_.read(snapshot, manifestLocation.manifest_page_id);
		const VectorManifestPage page = VectorManifestPage::open(contents.owner, contents.payload);
		const uint8_t invalid = static_cast<uint8_t>(contents.pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
								static_cast<uint8_t>(contents.owner.flags != 0U) |
								static_cast<uint8_t>(contents.owner.committed_lsn != expectedOwnerLsn) |
								static_cast<uint8_t>(page.manifest().index_id != indexId) |
								static_cast<uint8_t>(page.manifest().page_id != manifestLocation.manifest_page_id) |
								static_cast<uint8_t>(page.manifest().generation != manifestLocation.generation);
		snapshot.release();
		if (invalid != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable vector manifest retirement does not match its live owner and directory identity");
		}
		const ExistingPoolPageRetirementPreparer preparer = [this, indexId, expectedOwnerLsn,
															 checkpointLsn](const CatalogEntry &pool,
																			const PageLocation &location) {
			const std::array<SharedPagePoolPreparedRetirement, 1> retirements{
					{{location, indexId, PageOwnerKind::POK_VECTOR_MANIFEST, expectedOwnerLsn}}};
			allocator_.prepareMutationPages(pool, {}, {}, {}, {}, {}, checkpointLsn, retirements);
		};
		return metadata_.retireVectorManifestPageInExistingPool(contents.pool, contents.location, manifestLocation,
																checkpointLsn, preparer, options);
	}

	VectorManifestPage VectorManifestStore::read(const MetadataSnapshotPin &snapshot, uint64_t indexId) const {
		const VectorManifestLocation location = metadata_.findVectorManifest(snapshot, indexId);
		const SharedPagePoolContents contents = allocator_.read(snapshot, location.manifest_page_id);
		const VectorManifestPage page = VectorManifestPage::open(contents.owner, contents.payload);
		const VectorManifestLocation actual{page.manifest().index_id, page.manifest().page_id,
											page.manifest().generation};
		if (actual != location) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable vector manifest page disagrees with its Manifest Directory mapping");
		}
		return page;
	}

	SharedPagePoolAllocation VectorManifestStore::evacuateExpected(MetadataSnapshotPin &snapshot,
																   const VectorManifestLocation &expected,
																   SegmentId destinationSegmentId,
																   LogSequenceNumber checkpointLsn,
																   const MetadataPublishOptions &options) {
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, expected.manifest_page_id);
			const CatalogEntry target = metadata_.findCatalog(snapshot, destinationSegmentId);
			const VectorManifestPage page = VectorManifestPage::open(source.owner, source.payload);
			const uint8_t invalidSource =
					static_cast<uint8_t>(source.pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
					static_cast<uint8_t>(source.owner.page_id != expected.manifest_page_id) |
					static_cast<uint8_t>(source.owner.owner_id != expected.index_id) |
					static_cast<uint8_t>(source.owner.kind != PageOwnerKind::POK_VECTOR_MANIFEST) |
					static_cast<uint8_t>(source.owner.generation != expected.generation) |
					static_cast<uint8_t>(page.manifest().index_id != expected.index_id) |
					static_cast<uint8_t>(page.manifest().page_id != expected.manifest_page_id) |
					static_cast<uint8_t>(page.manifest().generation != expected.generation) |
					static_cast<uint8_t>(target.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
					static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(snapshot, target.physical_slot));
			if (invalidSource != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Vector Manifest evacuation requires a current compatible manifest and pool");
			}
			PageOwnerHeader destinationOwner = source.owner;
			destinationOwner.generation = target.generation;
			destinationOwner.committed_lsn = checkpointLsn;
			const VectorManifestPage relocated = page.rebindOwner(destinationOwner);
			validateReferencedCodebook(relocated.manifest());
			validateReferencedPqMapping(relocated.manifest());
			validateReferencedAdjacencyMapping(relocated.manifest());
			const std::vector<std::byte> payload = relocated.serialize();
			snapshot.release();

			SharedPagePoolEvacuationRequest request;
			request.source_segment_id = source.pool.segment_id;
			request.source_generation = source.pool.generation;
			request.destination_segment_id = destinationSegmentId;
			request.destination_generation = target.generation;
			request.page_id = expected.manifest_page_id;
			request.expected_owner_lsn = source.owner.committed_lsn;
			request.page_owner_id = expected.index_id;
			request.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
			request.page_free_bytes = 0;
			request.payload = payload;
			const VectorManifestLocation replacement{expected.index_id, expected.manifest_page_id, target.generation};
			return allocator_.evacuateVectorManifestPage(request, expected, replacement, checkpointLsn, options);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

	SharedPagePoolAllocation VectorManifestStore::evacuate(uint64_t indexId, SegmentId destinationSegmentId,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		if (indexId == 0U || !destinationSegmentId.isValid() || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest evacuation requires an index, destination pool, and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const VectorManifestLocation expected = metadata_.findVectorManifest(snapshot, indexId);
		return evacuateExpected(snapshot, expected, destinationSegmentId, checkpointLsn, options);
	}

	SharedPagePoolAllocation VectorManifestStore::evacuatePage(PageId manifestPageId, SegmentId destinationSegmentId,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		if (!manifestPageId.isValid() || !destinationSegmentId.isValid() || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest evacuation requires a page ID, destination pool, and checkpoint LSN");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			const SharedPagePoolContents source = allocator_.read(snapshot, manifestPageId);
			const VectorManifestPage page = VectorManifestPage::open(source.owner, source.payload);
			const VectorManifestLocation expected = metadata_.findVectorManifest(snapshot, page.manifest().index_id);
			const uint8_t invalid = static_cast<uint8_t>(expected.manifest_page_id != manifestPageId) |
									static_cast<uint8_t>(expected.generation != page.manifest().generation);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector Manifest page does not match its durable directory identity");
			}
			return evacuateExpected(snapshot, expected, destinationSegmentId, checkpointLsn, options);
		} catch (...) {
			snapshot.release();
			throw;
		}
	}

} // namespace graph::storage
