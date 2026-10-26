/**
 * @file RelocatableMetadataStoreVectorRebuild.cpp
 * @brief Atomic metadata publication for offline Vector derived-state rebuilds.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] bool hasSameCatalogEntry(const CatalogEntry &left, const CatalogEntry &right) {
			return left.segment_id == right.segment_id && left.physical_slot == right.physical_slot &&
				   left.kind == right.kind && left.owner_id == right.owner_id && left.generation == right.generation &&
				   left.state == right.state && left.committed_lsn == right.committed_lsn;
		}

	} // namespace

	Superblock RelocatableMetadataStore::publishOfflineVectorDerivedRebuild(
			const OfflineVectorDerivedRebuildMetadataMutation &mutation, LogSequenceNumber checkpointLsn,
			const OfflineVectorDerivedRebuildMetadataPreparer &preparer, const MetadataPublishOptions &options) {
		const NativeGraphIndexTreeMutation &mapping = mutation.mapping_tree;
		const NativeGraphIndexTreeMutation &adjacencyMapping = mutation.adjacency_mapping_tree;
		const bool replacesExistingMapping = mutation.expected_mapping_root.has_value();
		const bool hasAdjacency = !mutation.adjacency_pools.empty();
		const bool replacesExistingAdjacencyMapping = mutation.expected_adjacency_mapping_root.has_value();
		const bool retiresDeltaMapping = mutation.expected_delta_mapping_root.has_value();
		const uint8_t malformed =
				static_cast<uint8_t>(mutation.index_id == 0U) | static_cast<uint8_t>(!checkpointLsn.isValid()) |
				static_cast<uint8_t>(!preparer) |
				static_cast<uint8_t>(!mutation.expected_manifest_pool.segment_id.isValid()) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.owner_id != mutation.index_id) |
				static_cast<uint8_t>(mutation.expected_manifest_pool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!mutation.expected_manifest_location.page_id.isValid()) |
				static_cast<uint8_t>(mutation.expected_manifest_location.segment_id !=
									 mutation.expected_manifest_pool.segment_id) |
				static_cast<uint8_t>(mutation.expected_manifest_location.generation !=
									 mutation.expected_manifest_pool.generation) |
				static_cast<uint8_t>(mutation.expected_manifest.index_id != mutation.index_id) |
				static_cast<uint8_t>(mutation.expected_manifest.manifest_page_id !=
									 mutation.expected_manifest_location.page_id) |
				static_cast<uint8_t>(mutation.expected_manifest.generation !=
									 mutation.expected_manifest_location.generation) |
				static_cast<uint8_t>(!mutation.expected_manifest_owner_lsn.isValid()) |
				static_cast<uint8_t>(mutation.codebook_reservations.empty() != mutation.codebook_extents.empty()) |
				static_cast<uint8_t>(!mutation.codebook_reservations.empty() &&
									 (!mutation.codebook_first_slot.isValid() ||
									  !mutation.codebook_last_slot.isValid() ||
									  mutation.codebook_first_slot > mutation.codebook_last_slot)) |
				static_cast<uint8_t>(mutation.pq_pools.empty()) | static_cast<uint8_t>(mapping.index_id == 0U) |
				static_cast<uint8_t>(mapping.index_id != 0U && !VectorPqMapping::isMappingIndexId(mapping.index_id)) |
				static_cast<uint8_t>(mapping.expected_root.has_value()) | static_cast<uint8_t>(mapping.retire_root) |
				static_cast<uint8_t>(!mapping.pool_requests.empty()) |
				static_cast<uint8_t>(mapping.new_pool_requests.empty()) |
				static_cast<uint8_t>(!mapping.replacement_root.has_value()) |
				static_cast<uint8_t>(mapping.replacement_root.has_value() &&
									 !mapping.replacement_root->newly_reserved) |
				static_cast<uint8_t>(mapping.stable_replacement_root.has_value()) |
				static_cast<uint8_t>(replacesExistingMapping &&
									 (mutation.expected_mapping_root->index_id != mapping.index_id ||
									  !mutation.expected_mapping_root->root_page_id.isValid() ||
									  !mutation.expected_mapping_root->root_generation.isValid())) |
				static_cast<uint8_t>(!replacesExistingMapping &&
									 (!mutation.pq_retirements.empty() || !mutation.mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(replacesExistingMapping &&
									 (mutation.pq_retirements.empty() || mutation.mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.index_id == 0U) |
				static_cast<uint8_t>(hasAdjacency &&
									 !VectorAdjacencyMapping::isMappingIndexId(adjacencyMapping.index_id)) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.expected_root.has_value()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.retire_root) |
				static_cast<uint8_t>(hasAdjacency && !adjacencyMapping.pool_requests.empty()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.new_pool_requests.empty()) |
				static_cast<uint8_t>(hasAdjacency && !adjacencyMapping.replacement_root.has_value()) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.replacement_root.has_value() &&
									 !adjacencyMapping.replacement_root->newly_reserved) |
				static_cast<uint8_t>(hasAdjacency && adjacencyMapping.stable_replacement_root.has_value()) |
				static_cast<uint8_t>(replacesExistingAdjacencyMapping &&
									 (mutation.expected_adjacency_mapping_root->index_id != adjacencyMapping.index_id ||
									  !mutation.expected_adjacency_mapping_root->root_page_id.isValid() ||
									  !mutation.expected_adjacency_mapping_root->root_generation.isValid())) |
				static_cast<uint8_t>(hasAdjacency && !replacesExistingAdjacencyMapping &&
									 (!mutation.adjacency_retirements.empty() ||
									  !mutation.adjacency_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(replacesExistingAdjacencyMapping &&
									 (mutation.adjacency_retirements.empty() ||
									  mutation.adjacency_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(!hasAdjacency &&
									 (!mutation.adjacency_retirements.empty() || replacesExistingAdjacencyMapping ||
									  !mutation.adjacency_mapping_retirement_pools.empty() ||
									  adjacencyMapping.index_id != 0U || !adjacencyMapping.new_pool_requests.empty())) |
				static_cast<uint8_t>(
						retiresDeltaMapping &&
						(mutation.expected_delta_mapping_root->index_id == 0U ||
						 !mutation.expected_delta_mapping_root->root_page_id.isValid() ||
						 !mutation.expected_delta_mapping_root->root_generation.isValid() ||
						 !VectorDeltaMapping::isMappingIndexId(mutation.expected_delta_mapping_root->index_id) ||
						 mutation.delta_mapping_retirement_pools.empty())) |
				static_cast<uint8_t>(!retiresDeltaMapping && !mutation.delta_mapping_retirement_pools.empty());
		if (malformed != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"offline PQ rebuild has an invalid Manifest fence, Codebook candidate, or mapping-tree plan");
		}

		size_t totalPqPages = 0U;
		for (const VectorPqMaterializationPoolRequest &request: mutation.pq_pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPqPages) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid PQ pool placement or page count");
			}
			totalPqPages += request.page_count;
		}
		size_t totalAdjacencyPages = 0U;
		for (const VectorAdjacencyMaterializationPoolRequest &request: mutation.adjacency_pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalAdjacencyPages) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline Vector rebuild has an invalid adjacency pool placement or page count");
			}
			totalAdjacencyPages += request.page_count;
		}
		for (const NewIndexTreePoolRequest &request: mapping.new_pool_requests) {
			const uint8_t invalidPool =
					static_cast<uint8_t>(!request.first_slot.isValid()) |
					static_cast<uint8_t>(!request.last_slot.isValid()) |
					static_cast<uint8_t>(request.first_slot > request.last_slot) |
					static_cast<uint8_t>(request.owner_id != mapping.index_id) |
					static_cast<uint8_t>(request.appended_page_count == 0U) |
					static_cast<uint8_t>(request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid first-use mapping-tree pool request");
			}
		}
		for (const NewIndexTreePoolRequest &request: adjacencyMapping.new_pool_requests) {
			const uint8_t invalidPool =
					static_cast<uint8_t>(!request.first_slot.isValid()) |
					static_cast<uint8_t>(!request.last_slot.isValid()) |
					static_cast<uint8_t>(request.first_slot > request.last_slot) |
					static_cast<uint8_t>(request.owner_id != adjacencyMapping.index_id) |
					static_cast<uint8_t>(request.appended_page_count == 0U) |
					static_cast<uint8_t>(request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline Vector rebuild has an invalid adjacency mapping-tree pool request");
			}
		}
		if (mapping.replacement_root->pool_ordinal >= mapping.new_pool_requests.size() ||
			mapping.replacement_root->appended_page_ordinal >=
					mapping.new_pool_requests[mapping.replacement_root->pool_ordinal].appended_page_count) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "offline PQ rebuild mapping-tree root does not name a candidate PageId");
		}
		if (hasAdjacency &&
			(adjacencyMapping.replacement_root->pool_ordinal >= adjacencyMapping.new_pool_requests.size() ||
			 adjacencyMapping.replacement_root->appended_page_ordinal >=
					 adjacencyMapping.new_pool_requests[adjacencyMapping.replacement_root->pool_ordinal]
							 .appended_page_count)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"offline Vector rebuild adjacency mapping-tree root does not name a candidate PageId");
		}

		uint64_t previousPqRetirementPageId = 0U;
		std::set<uint64_t> retiredPageIds;
		for (const PageRetirement &retirement: mutation.pq_retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousPqRetirementPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id != mutation.index_id) |
					static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_PQ) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid or non-canonical retired PQ source page");
			}
			previousPqRetirementPageId = location.page_id.value();
		}
		uint64_t previousAdjacencyRetirementPageId = 0U;
		for (const PageRetirement &retirement: mutation.adjacency_retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousAdjacencyRetirementPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id != (mutation.index_id | (UINT64_C(1) << 63U))) |
					static_cast<uint8_t>(retirement.expected_owner_kind != PageOwnerKind::POK_VECTOR_ADJACENCY) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"offline Vector rebuild has an invalid or non-canonical retired adjacency source page");
			}
			previousAdjacencyRetirementPageId = location.page_id.value();
		}

		uint64_t previousRetirementSegmentId = 0U;
		bool retiresExpectedMappingRoot = false;
		for (const IndexTreeMutationPoolRequest &request: mutation.mapping_retirement_pools) {
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.segment_id.value() <= previousRetirementSegmentId) |
					static_cast<uint8_t>(!request.expected_replacements.empty()) |
					static_cast<uint8_t>(request.appended_page_count != 0U) |
					static_cast<uint8_t>(request.expected_retirements.empty());
			if (invalidRequest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "offline PQ rebuild has an invalid or unordered mapping-tree retirement pool");
			}
			uint64_t previousRetirementPageId = 0U;
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation &location = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(!location.page_id.isValid()) |
						static_cast<uint8_t>(location.page_id.value() <= previousRetirementPageId) |
						static_cast<uint8_t>(location.segment_id != request.segment_id) |
						static_cast<uint8_t>(!location.generation.isValid()) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
						static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
				if (invalidRetirement != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "offline PQ rebuild has an invalid, duplicate, or non-canonical mapping-tree "
										  "retirement page");
				}
				if (replacesExistingMapping && location.page_id == mutation.expected_mapping_root->root_page_id &&
					location.generation == mutation.expected_mapping_root->root_generation) {
					retiresExpectedMappingRoot = true;
				}
				previousRetirementPageId = location.page_id.value();
			}
			previousRetirementSegmentId = request.segment_id.value();
		}
		if (replacesExistingMapping && !retiresExpectedMappingRoot) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "offline PQ rebuild replacement does not retire its expected mapping-tree root");
		}
		const auto validateAdditionalMappingRetirements =
				[&retiredPageIds](std::span<const IndexTreeMutationPoolRequest> requests,
								  const std::optional<IndexRootLocation> &expectedRoot, const char *message) {
					uint64_t previousSegmentId = 0U;
					bool retiresExpectedRoot = !expectedRoot.has_value();
					for (const IndexTreeMutationPoolRequest &request: requests) {
						const uint8_t invalidRequest =
								static_cast<uint8_t>(!request.segment_id.isValid()) |
								static_cast<uint8_t>(request.segment_id.value() <= previousSegmentId) |
								static_cast<uint8_t>(!request.expected_replacements.empty()) |
								static_cast<uint8_t>(request.appended_page_count != 0U) |
								static_cast<uint8_t>(request.expected_retirements.empty());
						if (invalidRequest != 0U) {
							throw FormatException(FormatError::FERR_INVALID_HEADER, message);
						}
						uint64_t previousPageId = 0U;
						for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
							const PageLocation &location = retirement.location;
							const uint8_t invalidRetirement =
									static_cast<uint8_t>(!location.page_id.isValid()) |
									static_cast<uint8_t>(location.page_id.value() <= previousPageId) |
									static_cast<uint8_t>(location.segment_id != request.segment_id) |
									static_cast<uint8_t>(!location.generation.isValid()) |
									static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
									static_cast<uint8_t>(!retiredPageIds.insert(location.page_id.value()).second);
							if (invalidRetirement != 0U) {
								throw FormatException(FormatError::FERR_INVALID_HEADER, message);
							}
							if (expectedRoot.has_value() && location.page_id == expectedRoot->root_page_id &&
								location.generation == expectedRoot->root_generation) {
								retiresExpectedRoot = true;
							}
							previousPageId = location.page_id.value();
						}
						previousSegmentId = request.segment_id.value();
					}
					if (!retiresExpectedRoot) {
						throw FormatException(FormatError::FERR_INVALID_HEADER, message);
					}
				};
		if (hasAdjacency) {
			validateAdditionalMappingRetirements(
					mutation.adjacency_mapping_retirement_pools, mutation.expected_adjacency_mapping_root,
					"offline Vector rebuild does not retire its expected adjacency mapping-tree root");
		}
		if (retiresDeltaMapping) {
			validateAdditionalMappingRetirements(
					mutation.delta_mapping_retirement_pools, mutation.expected_delta_mapping_root,
					"offline Vector rebuild does not retire its expected delta mapping-tree root");
		}

		std::map<uint64_t, const CatalogSlotReservationRequest *> codebookReservationsBySegment;
		uint64_t codebookId = 0U;
		Generation codebookGeneration;
		for (const CatalogSlotReservationRequest &request: mutation.codebook_reservations) {
			const uint8_t invalidReservation =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(request.owner_id == 0U) |
					static_cast<uint8_t>(!request.segment_generation.isValid()) |
					static_cast<uint8_t>(request.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!request.require_absent) |
					static_cast<uint8_t>(codebookId != 0U && request.owner_id != codebookId) |
					static_cast<uint8_t>(codebookGeneration.isValid() &&
										  request.segment_generation != codebookGeneration) |
					static_cast<uint8_t>(!codebookReservationsBySegment
										   .emplace(request.segment_id.value(), &request)
										   .second);
			if (invalidReservation != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"offline PQ rebuild has duplicate or malformed immutable Codebook reservation identities");
			}
			codebookId = request.owner_id;
			codebookGeneration = request.segment_generation;
		}

		std::set<uint64_t> codebookExtentIds;
		size_t manifestExtentCount = 0U;
		for (const ExtentLocation &extent: mutation.codebook_extents) {
			const auto request = codebookReservationsBySegment.find(extent.segment_id.value());
			const bool isManifest = request != codebookReservationsBySegment.end() &&
					VectorCodebookExtentIdCodec::isManifestExtentForCodebook(extent.extent_id, request->second->owner_id);
			const bool isData = request != codebookReservationsBySegment.end() &&
					VectorCodebookExtentIdCodec::isDataExtentForSegment(extent.extent_id, extent.segment_id);
			const uint8_t invalidCodebook =
					static_cast<uint8_t>(request == codebookReservationsBySegment.end()) |
					static_cast<uint8_t>(request != codebookReservationsBySegment.end() &&
										  extent.generation != request->second->segment_generation) |
					static_cast<uint8_t>(!extent.extent_id.isValid()) |
					static_cast<uint8_t>(extent.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(extent.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!codebookExtentIds.insert(extent.extent_id.value()).second) |
					static_cast<uint8_t>(!isManifest && !isData);
			if (invalidCodebook != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"offline PQ rebuild has duplicate or malformed immutable Codebook extent identities");
			}
			manifestExtentCount += isManifest ? 1U : 0U;
		}
		if (!mutation.codebook_extents.empty() && manifestExtentCount != 1U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
							  "offline PQ rebuild Codebook candidate does not contain exactly one manifest extent");
		}

		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
		const VectorManifestLocation visibleManifest = locations_.findVectorManifest(current, mutation.index_id);
		const PageLocation visibleManifestPage =
				locations_.findPage(current, mutation.expected_manifest_location.page_id);
		const CatalogEntry visibleManifestPool = catalog_.find(current, visibleManifestPage.segment_id);
		const std::optional<IndexRootLocation> visibleMappingRoot =
				locations_.findIndexRootOptional(current, mapping.index_id);
		const std::optional<IndexRootLocation> visibleAdjacencyMappingRoot =
				hasAdjacency ? locations_.findIndexRootOptional(current, adjacencyMapping.index_id) : std::nullopt;
		const std::optional<IndexRootLocation> visibleDeltaMappingRoot =
				retiresDeltaMapping
						? locations_.findIndexRootOptional(current, mutation.expected_delta_mapping_root->index_id)
						: std::nullopt;
		const uint8_t stale =
				static_cast<uint8_t>(visibleManifest != mutation.expected_manifest) |
				static_cast<uint8_t>(visibleManifestPage != mutation.expected_manifest_location) |
				static_cast<uint8_t>(!hasSameCatalogEntry(visibleManifestPool, mutation.expected_manifest_pool)) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visibleManifestPool.physical_slot)) |
				static_cast<uint8_t>(
						replacesExistingMapping &&
						(!visibleMappingRoot.has_value() || *visibleMappingRoot != *mutation.expected_mapping_root)) |
				static_cast<uint8_t>(!replacesExistingMapping && visibleMappingRoot.has_value());
		const uint8_t derivedStale =
				static_cast<uint8_t>(hasAdjacency && replacesExistingAdjacencyMapping &&
									 (!visibleAdjacencyMappingRoot.has_value() ||
									  *visibleAdjacencyMappingRoot != *mutation.expected_adjacency_mapping_root)) |
				static_cast<uint8_t>(hasAdjacency && !replacesExistingAdjacencyMapping &&
									 visibleAdjacencyMappingRoot.has_value()) |
				static_cast<uint8_t>(retiresDeltaMapping &&
									 (!visibleDeltaMappingRoot.has_value() ||
									  *visibleDeltaMappingRoot != *mutation.expected_delta_mapping_root));
		if (stale != 0U || derivedStale != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild has a stale Manifest fence or mapping-root transition");
		}

		std::map<uint64_t, ExistingPoolPageRetirementBatch> pqRetirementBatchesBySegment;
		for (const PageRetirement &retirement: mutation.pq_retirements) {
			const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
										   static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_PQ_POOL) |
										   static_cast<uint8_t>(pool.owner_id != mutation.index_id) |
										   static_cast<uint8_t>(pool.generation != retirement.location.generation) |
										   static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										   static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has a stale, foreign, or unavailable retired PQ source page");
			}
			auto [iterator, inserted] = pqRetirementBatchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}
		std::map<uint64_t, ExistingPoolPageRetirementBatch> adjacencyRetirementBatchesBySegment;
		for (const PageRetirement &retirement: mutation.adjacency_retirements) {
			const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
										   static_cast<uint8_t>(pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL) |
										   static_cast<uint8_t>(pool.owner_id != mutation.index_id) |
										   static_cast<uint8_t>(pool.generation != retirement.location.generation) |
										   static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										   static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild has a stale, foreign, or unavailable retired adjacency source page");
			}
			auto [iterator, inserted] = adjacencyRetirementBatchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}
		// Validate the durable sources before the callback can write a replacement
		// suffix. A metadata route alone cannot prove that its typed owner is still
		// current, and discovering this after writes would leave avoidable debris.
		const auto authenticateDerivedRetirements = [this, &current](
															 const CatalogEntry &pool,
															 std::span<const PageRetirement> retirements,
															 uint64_t expectedOwnerId, PageOwnerKind expectedOwnerKind) {
			std::vector<SharedPagePoolPreparedRetirement> expected;
			expected.reserve(retirements.size());
			for (const PageRetirement &retirement: retirements) {
				expected.push_back(
						{retirement.location, expectedOwnerId, expectedOwnerKind, retirement.expected_owner_lsn});
			}
			SharedPagePoolAllocator::validateRetirementSourcesUnderWrite(*io_, current.data_region_offset, pool, expected);
		};
		for (const auto &[segmentId, retirementBatch]: pqRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			authenticateDerivedRetirements(retirementBatch.pool, retirementBatch.retirements, mutation.index_id,
												 PageOwnerKind::POK_VECTOR_PQ);
		}
		for (const auto &[segmentId, retirementBatch]: adjacencyRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			authenticateDerivedRetirements(retirementBatch.pool, retirementBatch.retirements,
												 VectorAdjacencyPageStore::derivedOwnerId(mutation.index_id),
												 PageOwnerKind::POK_VECTOR_ADJACENCY);
		}

		const auto authenticateMappingRetirements = [this, &current](
														 const CatalogEntry &pool,
														 std::span<const IndexTreePageRetirement> retirements,
														 uint64_t expectedIndexId) {
			std::vector<SharedPagePoolPreparedRetirement> expected;
			expected.reserve(retirements.size());
			for (const IndexTreePageRetirement &retirement: retirements) {
				expected.push_back(
						{retirement.location, expectedIndexId, PageOwnerKind::POK_INDEX, retirement.expected_owner_lsn});
			}
			SharedPagePoolAllocator::validateRetirementSourcesUnderWrite(*io_, current.data_region_offset, pool,
																							 expected);
		};

		std::vector<IndexTreeMutationPoolBatch> mappingRetirementBatches;
		mappingRetirementBatches.reserve(mutation.mapping_retirement_pools.size());
		for (const IndexTreeMutationPoolRequest &request: mutation.mapping_retirement_pools) {
			const CatalogEntry pool = catalog_.find(current, request.segment_id);
			// Existing Index pools can be shared by several logical mapping trees.
			// The immutable retirement plan and page headers authenticate the tree;
			// the Catalog owner identifies only the allocation namespace.
			const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										static_cast<uint8_t>(pool.owner_id == 0U) |
										static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild requires current owned mapping-tree retirement pools");
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
				const uint8_t invalidCurrent = static_cast<uint8_t>(visible != retirement.location) |
											   static_cast<uint8_t>(retirement.location.generation != pool.generation);
				if (invalidCurrent != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild has a stale or foreign mapping-tree retirement page");
				}
			}
			authenticateMappingRetirements(pool, request.expected_retirements, mapping.index_id);
			mappingRetirementBatches.push_back({pool, {}, {}, {}, {}, false, request.expected_retirements});
		}
		const auto materializeTreeRetirementBatches = [this, &current, &authenticateMappingRetirements](
																		   std::span<const IndexTreeMutationPoolRequest> requests,
																		   uint64_t expectedIndexId, const char *message) {
			std::vector<IndexTreeMutationPoolBatch> result;
			result.reserve(requests.size());
			for (const IndexTreeMutationPoolRequest &request: requests) {
				const CatalogEntry pool = catalog_.find(current, request.segment_id);
				// `expectedIndexId` identifies the B+Tree pages, not the shared
				// allocation namespace that owns this Index pool.
				const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
											static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
					const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
					if (visible != retirement.location || retirement.location.generation != pool.generation) {
						throw FormatException(FormatError::FERR_INVALID_STATE, message);
					}
				}
				authenticateMappingRetirements(pool, request.expected_retirements, expectedIndexId);
				result.push_back({pool, {}, {}, {}, {}, false, request.expected_retirements});
			}
			return result;
		};
		std::vector<IndexTreeMutationPoolBatch> adjacencyMappingRetirementBatches;
		if (hasAdjacency) {
			adjacencyMappingRetirementBatches = materializeTreeRetirementBatches(
					mutation.adjacency_mapping_retirement_pools, adjacencyMapping.index_id,
					"offline Vector rebuild requires current owned adjacency mapping-tree retirement pools");
		}
		std::vector<IndexTreeMutationPoolBatch> deltaMappingRetirementBatches;
		if (retiresDeltaMapping) {
			deltaMappingRetirementBatches = materializeTreeRetirementBatches(
					mutation.delta_mapping_retirement_pools, mutation.expected_delta_mapping_root->index_id,
					"offline Vector rebuild requires current owned delta mapping-tree retirement pools");
		}
		for (size_t index = 0U; index < mutation.codebook_reservations.size(); ++index) {
			if (catalog_.findOptional(current, mutation.codebook_reservations[index].segment_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline PQ rebuild Codebook candidate reuses a reachable Catalog segment identity");
			}
			if (locations_.findExtentOptional(current, mutation.codebook_extents[index].extent_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild Codebook candidate reuses a reachable Extent identity");
			}
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        // This validated vector-only closure leaves primary Nodes, fields and
        // complete scan membership unchanged. Retain all descriptor roots while
        // publishing its new derived mappings and immutable codebook together.
        if (current.hybrid_property_descriptor_root.isValid()) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
		OfflineVectorDerivedRebuildMetadataMaterialization materialization;
		materialization.codebook_extents.reserve(mutation.codebook_reservations.size());
		materialization.pq_retirement_pools.reserve(pqRetirementBatchesBySegment.size());
		for (auto &[segmentId, retirementBatch]: pqRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			materialization.pq_retirement_pools.push_back(std::move(retirementBatch));
		}
		materialization.adjacency_retirement_pools.reserve(adjacencyRetirementBatchesBySegment.size());
		for (auto &[segmentId, retirementBatch]: adjacencyRetirementBatchesBySegment) {
			static_cast<void>(segmentId);
			materialization.adjacency_retirement_pools.push_back(std::move(retirementBatch));
		}
		materialization.mapping_retirement_pools = std::move(mappingRetirementBatches);
		materialization.adjacency_mapping_retirement_pools = std::move(adjacencyMappingRetirementBatches);
		materialization.delta_mapping_retirement_pools = std::move(deltaMappingRetirementBatches);
		std::vector<CatalogEntry> catalogEntries;
		catalogEntries.reserve(mutation.codebook_reservations.size() + mutation.pq_pools.size() +
							   mapping.new_pool_requests.size() + mutation.adjacency_pools.size() +
							   adjacencyMapping.new_pool_requests.size());
		for (const CatalogSlotReservationRequest &request: mutation.codebook_reservations) {
			const std::optional<PhysicalSlotId> slot = allocator_.prepareReserveFirstFree(
					batch, mutation.codebook_first_slot, mutation.codebook_last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for an immutable Codebook extent");
			}
			CatalogEntry entry{request.segment_id,		   *slot,		  request.kind, request.owner_id,
							   request.segment_generation, request.state, checkpointLsn};
			batch.observeLogicalSegmentId(entry.segment_id);
			materialization.codebook_extents.push_back(entry);
			catalogEntries.push_back(entry);
		}

		materialization.pq_pools.reserve(mutation.pq_pools.size());
		for (const VectorPqMaterializationPoolRequest &request: mutation.pq_pools) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for a PQ Vector page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_VECTOR_PQ_POOL,
							  mutation.index_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			materialization.pq_pools.push_back({pool, {}, {}});
			catalogEntries.push_back(pool);
		}
		materialization.adjacency_pools.reserve(mutation.adjacency_pools.size());
		for (const VectorAdjacencyMaterializationPoolRequest &request: mutation.adjacency_pools) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline Vector rebuild has no free physical slot for an adjacency page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_VECTOR_ADJACENCY_POOL,
							  mutation.index_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			materialization.adjacency_pools.push_back({pool, {}, {}});
			catalogEntries.push_back(pool);
		}

		materialization.mapping_tree_pools.reserve(mapping.new_pool_requests.size());
		for (const NewIndexTreePoolRequest &request: mapping.new_pool_requests) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild has no free physical slot for a PQ mapping-tree page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_INDEX_PAGE_POOL,
							  request.owner_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			IndexTreeMutationPoolBatch treePool;
			treePool.pool = pool;
			treePool.newly_reserved = true;
			treePool.appended_page_ids.reserve(request.appended_page_count);
			treePool.appended_locations.reserve(request.appended_page_count);
			materialization.mapping_tree_pools.push_back(std::move(treePool));
			catalogEntries.push_back(pool);
		}
		materialization.adjacency_mapping_tree_pools.reserve(adjacencyMapping.new_pool_requests.size());
		for (const NewIndexTreePoolRequest &request: adjacencyMapping.new_pool_requests) {
			const std::optional<PhysicalSlotId> slot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!slot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild has no free physical slot for an adjacency mapping-tree page pool");
			}
			CatalogEntry pool{batch.allocateLogicalSegmentId(),
							  *slot,
							  SegmentKind::SGK_INDEX_PAGE_POOL,
							  request.owner_id,
							  Generation(1U),
							  SegmentState::SGS_SEALED,
							  checkpointLsn};
			IndexTreeMutationPoolBatch treePool;
			treePool.pool = pool;
			treePool.newly_reserved = true;
			treePool.appended_page_ids.reserve(request.appended_page_count);
			treePool.appended_locations.reserve(request.appended_page_count);
			materialization.adjacency_mapping_tree_pools.push_back(std::move(treePool));
			catalogEntries.push_back(pool);
		}
		for (VectorPqMaterializationPoolBatch &pool: materialization.pq_pools) {
			const auto request = mutation.pq_pools[&pool - materialization.pq_pools.data()];
			pool.page_ids.reserve(request.page_count);
			pool.locations.reserve(request.page_count);
			for (size_t pageOrdinal = 0U; pageOrdinal < request.page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_pools.size(); ++poolOrdinal) {
			VectorAdjacencyMaterializationPoolBatch &pool = materialization.adjacency_pools[poolOrdinal];
			const VectorAdjacencyMaterializationPoolRequest &request = mutation.adjacency_pools[poolOrdinal];
			pool.page_ids.reserve(request.page_count);
			pool.locations.reserve(request.page_count);
			for (size_t pageOrdinal = 0U; pageOrdinal < request.page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.mapping_tree_pools.size(); ++poolOrdinal) {
			IndexTreeMutationPoolBatch &pool = materialization.mapping_tree_pools[poolOrdinal];
			const NewIndexTreePoolRequest &request = mapping.new_pool_requests[poolOrdinal];
			for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				pool.appended_page_ids.push_back(pageId);
				pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_mapping_tree_pools.size();
			 ++poolOrdinal) {
			IndexTreeMutationPoolBatch &pool = materialization.adjacency_mapping_tree_pools[poolOrdinal];
			const NewIndexTreePoolRequest &request = adjacencyMapping.new_pool_requests[poolOrdinal];
			for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				pool.appended_page_ids.push_back(pageId);
				pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
			}
		}
		const IndexTreeMutationPoolBatch &rootPool =
				materialization.mapping_tree_pools[mapping.replacement_root->pool_ordinal];
		materialization.mapping_root = {mapping.index_id,
										rootPool.appended_page_ids[mapping.replacement_root->appended_page_ordinal],
										rootPool.pool.generation};
		if (hasAdjacency) {
			const IndexTreeMutationPoolBatch &adjacencyRootPool =
					materialization.adjacency_mapping_tree_pools[adjacencyMapping.replacement_root->pool_ordinal];
			materialization.adjacency_mapping_root = {
					adjacencyMapping.index_id,
					adjacencyRootPool.appended_page_ids[adjacencyMapping.replacement_root->appended_page_ordinal],
					adjacencyRootPool.pool.generation};
		}
		materialization.manifest_replacement = {mutation.expected_manifest_location.page_id,
												mutation.expected_manifest_pool.segment_id, 0U,
												mutation.expected_manifest_pool.generation, 0U};
		std::ranges::sort(catalogEntries, {}, &CatalogEntry::segment_id);
		catalog_.prepareUpsert(batch, catalogEntries);

		const OfflineVectorDerivedRebuildMetadataMaterialization planned = materialization;
		preparer(materialization);
		const bool changedCodebookEntries =
				materialization.codebook_extents.size() != planned.codebook_extents.size() ||
				!std::ranges::equal(materialization.codebook_extents, planned.codebook_extents, hasSameCatalogEntry);
		const auto hasSamePqRetirementBatches = [](std::span<const ExistingPoolPageRetirementBatch> actual,
												   std::span<const ExistingPoolPageRetirementBatch> expected) {
			if (actual.size() != expected.size()) {
				return false;
			}
			for (size_t ordinal = 0U; ordinal < actual.size(); ++ordinal) {
				if (!hasSameCatalogEntry(actual[ordinal].pool, expected[ordinal].pool) ||
					actual[ordinal].retirements.size() != expected[ordinal].retirements.size()) {
					return false;
				}
				for (size_t retirementOrdinal = 0U; retirementOrdinal < actual[ordinal].retirements.size();
					 ++retirementOrdinal) {
					const PageRetirement &left = actual[ordinal].retirements[retirementOrdinal];
					const PageRetirement &right = expected[ordinal].retirements[retirementOrdinal];
					if (left.location != right.location || left.expected_owner_id != right.expected_owner_id ||
						left.expected_owner_kind != right.expected_owner_kind ||
						left.expected_owner_lsn != right.expected_owner_lsn) {
						return false;
					}
				}
			}
			return true;
		};
		const auto hasSameMappingRetirementBatches = [](std::span<const IndexTreeMutationPoolBatch> actual,
														std::span<const IndexTreeMutationPoolBatch> expected) {
			if (actual.size() != expected.size()) {
				return false;
			}
			for (size_t ordinal = 0U; ordinal < actual.size(); ++ordinal) {
				const IndexTreeMutationPoolBatch &left = actual[ordinal];
				const IndexTreeMutationPoolBatch &right = expected[ordinal];
				if (!hasSameCatalogEntry(left.pool, right.pool) || !left.expected_replacements.empty() ||
					!left.appended_page_ids.empty() || left.newly_reserved != right.newly_reserved ||
					left.expected_retirements != right.expected_retirements || !left.replacement_locations.empty() ||
					!left.appended_locations.empty()) {
					return false;
				}
			}
			return true;
		};
		if (changedCodebookEntries) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned Codebook identity");
		}
		if (materialization.mapping_root != planned.mapping_root ||
			materialization.pq_pools.size() != planned.pq_pools.size() ||
			materialization.mapping_tree_pools.size() != planned.mapping_tree_pools.size() ||
			materialization.adjacency_mapping_root != planned.adjacency_mapping_root ||
			materialization.adjacency_pools.size() != planned.adjacency_pools.size() ||
			materialization.adjacency_mapping_tree_pools.size() != planned.adjacency_mapping_tree_pools.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned mapping or pool identity");
		}
		if (!hasSamePqRetirementBatches(materialization.pq_retirement_pools, planned.pq_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned PQ retirement plan");
		}
		if (!hasSamePqRetirementBatches(materialization.adjacency_retirement_pools,
										planned.adjacency_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline Vector rebuild preparer changed a metadata-owned adjacency retirement plan");
		}
		if (!hasSameMappingRetirementBatches(materialization.mapping_retirement_pools,
											 planned.mapping_retirement_pools)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer changed a metadata-owned mapping-tree retirement plan");
		}
		if (!hasSameMappingRetirementBatches(materialization.adjacency_mapping_retirement_pools,
											 planned.adjacency_mapping_retirement_pools) ||
			!hasSameMappingRetirementBatches(materialization.delta_mapping_retirement_pools,
											 planned.delta_mapping_retirement_pools)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"offline Vector rebuild preparer changed a metadata-owned derived mapping-tree retirement plan");
		}

		size_t mappingPageCount = 0U;
		for (const IndexTreeMutationPoolBatch &pool: materialization.mapping_tree_pools) {
			mappingPageCount += pool.appended_page_ids.size();
		}
		size_t adjacencyMappingPageCount = 0U;
		for (const IndexTreeMutationPoolBatch &pool: materialization.adjacency_mapping_tree_pools) {
			adjacencyMappingPageCount += pool.appended_page_ids.size();
		}
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPqPages + totalAdjacencyPages + mappingPageCount + adjacencyMappingPageCount + 1U);
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				publicationFrames;
		std::vector<uint64_t> publicationOrder;
		const auto registerPublication = [&publicationFrames, &publicationOrder, checkpointLsn](const CatalogEntry &pool,
															 const PageLocation &location, uint64_t ownerId,
															 PageOwnerKind ownerKind) {
			auto [iterator, inserted] = publicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
				publicationOrder.push_back(pool.segment_id.value());
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline Vector rebuild reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, ownerId, ownerKind, checkpointLsn});
		};
		std::set<std::pair<uint64_t, uint16_t>> frames;
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.pq_pools.size(); ++poolOrdinal) {
			const VectorPqMaterializationPoolBatch &actual = materialization.pq_pools[poolOrdinal];
			const VectorPqMaterializationPoolBatch &expected = planned.pq_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.page_ids != expected.page_ids ||
				actual.locations.size() != expected.locations.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild preparer changed a PQ pool identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				// Candidate PageIds are writer-owned: the batch allocated fresh, unique
				// IDs and the identity check above prevents a preparer from changing them.
				if (invalidLocation != 0U ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline PQ rebuild preparer returned an invalid or duplicate PQ page frame");
				}
				publishedPages.push_back(location);
				registerPublication(actual.pool, location, mutation.index_id, PageOwnerKind::POK_VECTOR_PQ);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_pools.size(); ++poolOrdinal) {
			const VectorAdjacencyMaterializationPoolBatch &actual = materialization.adjacency_pools[poolOrdinal];
			const VectorAdjacencyMaterializationPoolBatch &expected = planned.adjacency_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.page_ids != expected.page_ids ||
				actual.locations.size() != expected.locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild preparer changed an adjacency pool identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"offline Vector rebuild preparer returned an invalid or duplicate adjacency frame");
				}
				publishedPages.push_back(location);
				registerPublication(actual.pool, location, VectorAdjacencyPageStore::derivedOwnerId(mutation.index_id),
										PageOwnerKind::POK_VECTOR_ADJACENCY);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.mapping_tree_pools.size(); ++poolOrdinal) {
			const IndexTreeMutationPoolBatch &actual = materialization.mapping_tree_pools[poolOrdinal];
			const IndexTreeMutationPoolBatch &expected = planned.mapping_tree_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.newly_reserved != expected.newly_reserved ||
				actual.appended_page_ids != expected.appended_page_ids ||
				actual.appended_locations.size() != expected.appended_locations.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "offline PQ rebuild preparer changed a mapping-tree identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.appended_locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.appended_page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"offline PQ rebuild preparer returned an invalid or duplicate mapping-tree frame");
				}
				publishedPages.push_back(location);
				registerPublication(actual.pool, location, mapping.index_id, PageOwnerKind::POK_INDEX);
			}
		}
		for (size_t poolOrdinal = 0U; poolOrdinal < materialization.adjacency_mapping_tree_pools.size();
			 ++poolOrdinal) {
			const IndexTreeMutationPoolBatch &actual = materialization.adjacency_mapping_tree_pools[poolOrdinal];
			const IndexTreeMutationPoolBatch &expected = planned.adjacency_mapping_tree_pools[poolOrdinal];
			if (!hasSameCatalogEntry(actual.pool, expected.pool) || actual.newly_reserved != expected.newly_reserved ||
				actual.appended_page_ids != expected.appended_page_ids ||
				actual.appended_locations.size() != expected.appended_locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"offline Vector rebuild preparer changed an adjacency mapping-tree identity or PageId shape");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
				const PageLocation &location = actual.appended_locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != actual.appended_page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != actual.pool.segment_id) |
						static_cast<uint8_t>(location.generation != actual.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				if (invalidLocation != 0U ||
					!frames.emplace(location.segment_id.value(), location.page_index).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "offline Vector rebuild preparer returned an invalid or duplicate adjacency "
										  "mapping-tree frame");
				}
				publishedPages.push_back(location);
				registerPublication(actual.pool, location, adjacencyMapping.index_id, PageOwnerKind::POK_INDEX);
			}
		}
		const PageLocation &manifest = materialization.manifest_replacement;
		const uint8_t invalidManifest =
				static_cast<uint8_t>(manifest.page_id != mutation.expected_manifest_location.page_id) |
				static_cast<uint8_t>(manifest.segment_id != mutation.expected_manifest_pool.segment_id) |
				static_cast<uint8_t>(manifest.generation != mutation.expected_manifest_pool.generation) |
				static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(manifest.page_index == mutation.expected_manifest_location.page_index) |
				static_cast<uint8_t>(manifest.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		if (invalidManifest != 0U || !frames.emplace(manifest.segment_id.value(), manifest.page_index).second) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "offline PQ rebuild preparer returned an invalid Manifest replacement frame");
		}
		publishedPages.push_back(manifest);
		registerPublication(mutation.expected_manifest_pool, manifest, mutation.index_id,
								PageOwnerKind::POK_VECTOR_MANIFEST);
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		// A rebuild callback owns typed serialization, but metadata owns reachability.
		// Authenticate only the candidate frames before any directory becomes durable.
		for (const uint64_t segmentId: publicationOrder) {
			const auto &publication = publicationFrames.at(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
					*io_, current.data_region_offset, publication.first, publication.second);
		}
		const std::vector<uint64_t> erasedPageIds(retiredPageIds.begin(), retiredPageIds.end());
		locations_.prepareReplacePages(batch, publishedPages, erasedPageIds);
		if (!mutation.codebook_extents.empty()) {
			locations_.prepareUpsertExtents(batch, mutation.codebook_extents);
		}
		std::array<IndexRootLocation, 2U> replacementRoots{materialization.mapping_root,
														   materialization.adjacency_mapping_root};
		const std::span<const IndexRootLocation> publishedRoots(replacementRoots.data(), hasAdjacency ? 2U : 1U);
		const std::array<uint64_t, 1U> erasedDeltaRoot{
				{retiresDeltaMapping ? mutation.expected_delta_mapping_root->index_id : 0U}};
		const std::span<const uint64_t> erasedRoots =
				retiresDeltaMapping ? std::span<const uint64_t>(erasedDeltaRoot) : std::span<const uint64_t>{};
		locations_.prepareReplaceIndexRoots(batch, publishedRoots, erasedRoots);
		const VectorManifestLocation replacementManifest{mutation.index_id, manifest.page_id, manifest.generation};
		locations_.prepareReplaceVectorManifests(batch, std::span(&replacementManifest, 1U), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

} // namespace graph::storage
