/**
 * @file test_RelocatableProcessCrash_GraphWrite.cpp
 * @brief Real child-process crash coverage for compound native graph writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;

namespace {

	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_ENTITY_CANDIDATE_SHORT_WRITE =
			"compound-graph-entity-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_PROPERTY_CANDIDATE_SHORT_WRITE =
			"compound-graph-property-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_LABEL_SET_CANDIDATE_SHORT_WRITE =
			"compound-graph-label-set-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_CANDIDATE_SHORT_WRITE =
			"compound-graph-adjacency-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_BULK_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"compound-graph-adjacency-bulk-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_SCALAR_INDEX_CANDIDATE_SHORT_WRITE =
			"compound-graph-scalar-index-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE =
			"compound-graph-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_COMPOUND_GRAPH_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC =
			"compound-graph-after-superblock-write-before-final-sync";
	constexpr std::string_view CRASH_CHILD_ENTITY_PAGE_EVACUATION_CANDIDATE = "entity-page-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_CANDIDATE =
			"entity-label-page-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_AFTER_PUBLICATION =
			"entity-label-page-evacuation-after-publication";
	constexpr std::string_view CRASH_CHILD_ENTITY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"entity-page-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_PROPERTY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"property-page-replacement-candidate-short-write";
	constexpr uint64_t CRASH_SCALAR_INDEX_ID = 17U;

	NativeGraphWritePlannerOptions graphWriteOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	PropertyCollectionRoot propertyRoot(uint64_t ownerId, uint32_t ownerType, int64_t value) {
		return {ownerId, ownerType, {{17U, PropertyValue(value)}}};
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	bool isCandidatePageWithOwnerKind(const void *bytes, size_t byteCount, PageOwnerKind ownerKind) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
			return PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind == ownerKind;
		} catch (const std::exception &) {
			return false;
		}
	}

	bool isCandidateIndexPageForId(const void *bytes, size_t byteCount, uint64_t indexId) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
			const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
			if (owner.kind != PageOwnerKind::POK_INDEX) {
				return false;
			}
			return BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes)).header().index_id ==
				   indexId;
		} catch (const std::exception &) {
			return false;
		}
	}

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableGraphWriteCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
										   const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_COMPOUND_GRAPH_ENTITY_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_PROPERTY_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_LABEL_SET_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_BULK_REPLACEMENT_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_SCALAR_INDEX_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_COMPOUND_GRAPH_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC &&
			mode != CRASH_CHILD_ENTITY_PAGE_EVACUATION_CANDIDATE &&
			mode != CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_CANDIDATE &&
			mode != CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_AFTER_PUBLICATION &&
			mode != CRASH_CHILD_ENTITY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_PROPERTY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
			return -1;
		}
		try {
			if (mode == CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_CANDIDATE ||
				mode == CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_AFTER_PUBLICATION) {
				auto file = std::make_shared<std::fstream>(
						databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 108;
				const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
				if (nativeHandle == INVALID_FILE_HANDLE) return 109;
				auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
				EntityPageStore entities(io, *metadata);
				NodeEntityRecord node;
				node.inline_label_ids = {7U, 9U, 11U};
				node.inline_label_count = 3U;
				const EntityPageNewPoolRequest source{
						SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
				const EntityPageNewPoolRequest destination{
						SegmentId(32U), 901U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
				if (!entities.allocateNode(source, 1001U, node, LogSequenceNumber(2U)).has_value() ||
					!entities.allocateNode(destination, 1002U, node, LogSequenceNumber(3U)).has_value()) {
					return 110;
				}
				SlottedPagePool pages(io, *metadata);
				EntityLabelSetStore labels(io, *metadata, pages);
				std::vector<uint64_t> overflowLabels;
				overflowLabels.reserve(RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
				for (uint64_t label = 7U; label < 7U + 2U * (RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U); label += 2U) {
					overflowLabels.push_back(label);
				}
				MetadataSnapshotPin beforeLabels = metadata->openSnapshot();
				EntityLabelSetWritePlan labelPlan;
				labelPlan.append_page = EntityLabelSetAppendRequest{SegmentId(31U), PageId(102U)};
				static_cast<void>(
						labels.updateNodeLabels(beforeLabels, 1001U, overflowLabels, labelPlan, LogSequenceNumber(4U)));
				beforeLabels.release();
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, PhysicalSlotId(41U)) + 2U * RSS_PAGE_SIZE;
				StorageIO::ScopedNativeWriteHookForTesting pauseAtRelocationBoundary(
						[mode, candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (mode == CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_AFTER_PUBLICATION && offset >= 0 &&
								static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							if (mode == CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_CANDIDATE &&
								static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(entities.evacuatePoolPage(snapshot, PageId(102U), SegmentId(32U), LogSequenceNumber(5U)));
				return 111;
			}
			if (mode == CRASH_CHILD_ENTITY_PAGE_EVACUATION_CANDIDATE) {
				auto file = std::make_shared<std::fstream>(
						databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 100;
				const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
				if (nativeHandle == INVALID_FILE_HANDLE) return 101;
				auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
				EntityPageStore entities(io, *metadata);
				NodeEntityRecord original;
				original.inline_label_ids = {7U, 9U, 11U};
				original.inline_label_count = 3U;
				const EntityPageNewPoolRequest source{
						SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
				const EntityPageNewPoolRequest destination{
						SegmentId(32U), 901U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
				if (!entities.allocateNode(source, 1001U, original, LogSequenceNumber(2U)).has_value() ||
					!entities.allocateNode(destination, 1002U, original, LogSequenceNumber(3U)).has_value()) {
					return 102;
				}
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, PhysicalSlotId(41U)) + 2U * RSS_PAGE_SIZE;
				StorageIO::ScopedNativeWriteHookForTesting pauseAtCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(entities.evacuateNodePage(snapshot, PageId(101U), SegmentId(32U), LogSequenceNumber(4U)));
				return 103;
			}
			if (mode == CRASH_CHILD_ENTITY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
				auto file = std::make_shared<std::fstream>(
						databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 104;
				const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
				if (nativeHandle == INVALID_FILE_HANDLE) return 105;
				auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
				EntityPageStore entities(io, *metadata);
				NodeEntityRecord original;
				original.inline_label_ids = {7U, 9U, 11U};
				original.inline_label_count = 3U;
				const EntityPageNewPoolRequest request{
						SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
				const auto allocated = entities.allocateNode(request, 1001U, original, LogSequenceNumber(2U));
				if (!allocated.has_value()) return 106;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, allocated->pool.physical_slot) + 2U * RSS_PAGE_SIZE;
				NodeEntityRecord replacement;
				replacement.inline_label_ids = {13U, 15U, 17U};
				replacement.inline_label_count = 3U;
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(entities.updateNode(snapshot, 1001U, replacement, LogSequenceNumber(3U)));
				return 107;
			}
			if (mode == CRASH_CHILD_PROPERTY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
				auto file = std::make_shared<std::fstream>(
						databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 108;
				const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
				if (nativeHandle == INVALID_FILE_HANDLE) return 109;
				auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
				EntityPageStore entities(io, *metadata);
				SlottedPagePool pages(io, *metadata);
				PropertyCollectionRootStore properties(pages, *metadata);
				const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
				const PropertyCollectionRoot original{1001U, Node::typeId, {{3U, PropertyValue("before crash")}}};
				MetadataSnapshotPin initialSnapshot = metadata->openSnapshot();
				const std::vector<EntityPropertyRootMutationPlan> initialPlans =
						properties.planEntityRootReplacements(initialSnapshot, std::span(&original, 1U), options);
				EntityPageBatchNewPool nodePool;
				nodePool.request = {SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U),
											PhysicalSlotId(40U)};
				nodePool.record_kind = EntityPageRecordKind::EPRK_NODE;
				nodePool.node_insertions.push_back({1001U, {}});
				const std::array nodePools{nodePool};
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						initialSnapshot, {}, {}, nodePools, initialPlans, properties, options, LogSequenceNumber(2U)));
				initialSnapshot.release();

				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
				const EntityPropertyLocation location = metadata->findEntityPropertyLocation(snapshot, key);
				const PageLocation routed = metadata->findPageLocation(snapshot, location.page_id);
				const CatalogEntry pool = metadata->findCatalog(snapshot, routed.segment_id);
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, pool.physical_slot) + 2U * RSS_PAGE_SIZE;
				const PropertyCollectionRoot replacement{1001U, Node::typeId, {{3U, PropertyValue("after crash")}}};
				const std::vector<EntityPropertyRootMutationPlan> replacementPlans =
						properties.planEntityRootReplacements(snapshot, std::span(&replacement, 1U), options);
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(entities.applyNativeGraphBatchTransaction(
						snapshot, {}, {}, {}, replacementPlans, properties, options, LogSequenceNumber(3U)));
				return 110;
			}
			const bool replacesExistingAdjacency =
					mode == CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_BULK_REPLACEMENT_CANDIDATE_SHORT_WRITE;
			auto file = std::make_shared<std::fstream>(databasePath,
					replacesExistingAdjacency ? std::ios::in | std::ios::out | std::ios::binary
												   : std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			if (!file->is_open())
				return 100;
			const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
			if (nativeHandle == INVALID_FILE_HANDLE)
				return 101;
			auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = replacesExistingAdjacency ? RelocatableMetadataStore::open(io)
																		 : RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			EntityPageStore entities(io, *metadata);
			SlottedPagePool slottedPages(io, *metadata);
			BTreePageStore indexes(slottedPages, *metadata);
			EntityLabelSetStore labels(io, *metadata, slottedPages);
			PropertyCollectionRootStore properties(slottedPages, *metadata);
			NativeGraphWritePlannerOptions options = graphWriteOptions();
			if (replacesExistingAdjacency) {
				options.global_adjacency_bulk_rebuild_entry_threshold = 2U;
			}
			NativeGraphWritePlanner planner(*metadata, entities, labels, properties, indexes, options);
			NativeGraphWriteTransaction transaction = planner.beginTransaction();
			if (replacesExistingAdjacency) {
				const uint64_t firstTarget = transaction.createNode(std::array<uint64_t, 0U>{});
				const uint64_t secondTarget = transaction.createNode(std::array<uint64_t, 0U>{});
				static_cast<void>(transaction.createEdge(1U, firstTarget, 21U));
				static_cast<void>(transaction.createEdge(1U, secondTarget, 23U));
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (isCandidateIndexPageForId(bytes, byteCount, RSS_GLOBAL_ADJACENCY_INDEX_ID)) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(transaction.commit());
				return 102;
			}
			const std::vector<uint64_t> firstNodeLabels =
					mode == CRASH_CHILD_COMPOUND_GRAPH_LABEL_SET_CANDIDATE_SHORT_WRITE ||
							mode == CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE
							? std::vector<uint64_t>{7U, 8U, 9U, 10U}
							: std::vector<uint64_t>{7U};
			const uint64_t firstNode = transaction.createNode(firstNodeLabels);
			const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
			const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
			transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
			transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
			if (mode == CRASH_CHILD_COMPOUND_GRAPH_SCALAR_INDEX_CANDIDATE_SHORT_WRITE ||
				mode == CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE ||
				mode == CRASH_CHILD_COMPOUND_GRAPH_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC) {
				const std::array<BTreePageEntry, 1U> entries{{{{std::byte{'s'}}, firstNode}}};
				transaction.stageScalarIndexCreate(indexes, CRASH_SCALAR_INDEX_ID, entries);
			}
			uint64_t entityCandidateOffset = 0U;
			uint64_t metadataPoolOffset = 0U;
			if (mode == CRASH_CHILD_COMPOUND_GRAPH_ENTITY_CANDIDATE_SHORT_WRITE) {
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				entityCandidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
												   snapshot.superblock().data_region_offset, PhysicalSlotId(40U)) +
												static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME) * RSS_PAGE_SIZE;
				snapshot.release();
			}
			if (mode == CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE) {
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
			}
			const auto isTargetCandidate = [mode, entityCandidateOffset, metadataPoolOffset](const void *bytes,
																								  size_t byteCount, pwrite_off_t offset) {
				if (mode == CRASH_CHILD_COMPOUND_GRAPH_ENTITY_CANDIDATE_SHORT_WRITE) {
					return static_cast<uint64_t>(offset) == entityCandidateOffset && byteCount == RSS_PAGE_SIZE;
				}
				if (mode == CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					return byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
							absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE;
				}
				if (mode == CRASH_CHILD_COMPOUND_GRAPH_SCALAR_INDEX_CANDIDATE_SHORT_WRITE) {
					return isCandidateIndexPageForId(bytes, byteCount, CRASH_SCALAR_INDEX_ID);
				}
				const PageOwnerKind expected = mode == CRASH_CHILD_COMPOUND_GRAPH_PROPERTY_CANDIDATE_SHORT_WRITE
																 ? PageOwnerKind::POK_PROPERTY
																 : mode == CRASH_CHILD_COMPOUND_GRAPH_LABEL_SET_CANDIDATE_SHORT_WRITE
																			   ? PageOwnerKind::POK_ENTITY_LABEL
																			   : PageOwnerKind::POK_INDEX;
				return isCandidatePageWithOwnerKind(bytes, byteCount, expected);
			};
			if (mode == CRASH_CHILD_COMPOUND_GRAPH_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC) {
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterSuperblockWrite(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount)) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(transaction.commit());
			} else {
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[isTargetCandidate, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																					   pwrite_off_t offset) {
							if (isTargetCandidate(bytes, byteCount, offset)) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(transaction.commit());
			}
			return 102;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 103;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashGraphWriteTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
					   ("relocatable_graph_write_process_crash_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	void killCandidateAndAssertNoPartialGraphState(std::string_view mode, bool includesScalarIndex = false,
																 bool includesLabelSet = false) {
		CrashChild child(databasePath, markerPath, mode);
		ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
		child.terminate();

		auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		EntityPageStore entities(io, *metadata);
		SlottedPagePool slottedPages(io, *metadata);
		BTreePageStore indexes(slottedPages, *metadata);
		EntityLabelSetStore labels(io, *metadata, slottedPages);
		PropertyCollectionRootStore properties(slottedPages, *metadata);
		MetadataSnapshotPin recovered = metadata->openSnapshot();
		EXPECT_EQ(recovered.superblock().generation, Generation(4U));
		EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 1U)), FormatException);
		EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 2U)), FormatException);
		EXPECT_THROW(static_cast<void>(entities.readEdge(recovered, 1U)), FormatException);
		EXPECT_FALSE(metadata->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		if (includesScalarIndex) {
			EXPECT_FALSE(metadata->findIndexRootOptional(recovered, CRASH_SCALAR_INDEX_ID).has_value());
		}
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, 1U})
						 .has_value());
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, 1U})
						 .has_value());
		recovered.release();

		NativeGraphWritePlanner planner(*metadata, entities, labels, properties, indexes, graphWriteOptions());
		NativeGraphWriteTransaction retry = planner.beginTransaction();
		const std::vector<uint64_t> retryFirstNodeLabels =
				includesLabelSet ? std::vector<uint64_t>{7U, 8U, 9U, 10U} : std::vector<uint64_t>{7U};
		const uint64_t firstNode = retry.createNode(retryFirstNodeLabels);
		const uint64_t secondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
		const uint64_t edge = retry.createEdge(firstNode, secondNode, 11U);
		retry.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
		retry.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
		if (includesScalarIndex) {
			const std::array<BTreePageEntry, 1U> entries{{{{std::byte{'s'}}, firstNode}}};
			retry.stageScalarIndexCreate(indexes, CRASH_SCALAR_INDEX_ID, entries);
		}
		ASSERT_TRUE(retry.commit().committed());

		MetadataSnapshotPin published = metadata->openSnapshot();
		GlobalAdjacencyIndex adjacency(indexes, *metadata);
		EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
				  std::vector<uint64_t>{edge});
		EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
				  std::vector<uint64_t>{edge});
		if (includesScalarIndex) {
			BTreeIndex scalarIndex(CRASH_SCALAR_INDEX_ID, indexes, *metadata);
			const std::array<std::byte, 1U> key = {std::byte{'s'}};
			EXPECT_EQ(scalarIndex.find(published, key), std::optional<uint64_t>(firstNode));
		}
		EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
				  propertyRoot(firstNode, Node::typeId, 42));
		EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
				  propertyRoot(edge, Edge::typeId, 84));
		if (includesLabelSet) {
			EXPECT_EQ(labels.readNodeLabels(published, entities.readNode(published, firstNode)), retryFirstNodeLabels);
		}
		published.release();
	}
};

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphEntityCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_COMPOUND_GRAPH_ENTITY_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	BTreePageStore indexes(slottedPages, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 2U)), FormatException);
	EXPECT_THROW(static_cast<void>(entities.readEdge(recovered, 1U)), FormatException);
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, 1U})
						 .has_value());
	EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, 1U})
						 .has_value());
	recovered.release();

	NativeGraphWritePlanner planner(*metadata, entities, labels, properties, indexes, graphWriteOptions());
	NativeGraphWriteTransaction retry = planner.beginTransaction();
	const uint64_t firstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = retry.createEdge(firstNode, secondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata->openSnapshot();
	GlobalAdjacencyIndex adjacency(indexes, *metadata);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
			  propertyRoot(firstNode, Node::typeId, 42));
	EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
			  propertyRoot(edge, Edge::typeId, 84));
	published.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphPropertyCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	killCandidateAndAssertNoPartialGraphState(CRASH_CHILD_COMPOUND_GRAPH_PROPERTY_CANDIDATE_SHORT_WRITE);
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphLabelSetCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	killCandidateAndAssertNoPartialGraphState(CRASH_CHILD_COMPOUND_GRAPH_LABEL_SET_CANDIDATE_SHORT_WRITE, false, true);
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphAdjacencyCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	killCandidateAndAssertNoPartialGraphState(CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_CANDIDATE_SHORT_WRITE);
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringBulkAdjacencyReplacementCandidateShortWriteRetainsOldTreeAndCanRetry) {
	{
		auto file = std::make_shared<std::fstream>(
				databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		EntityPageStore entities(io, *metadata);
		SlottedPagePool slottedPages(io, *metadata);
		BTreePageStore indexes(slottedPages, *metadata);
		EntityLabelSetStore labels(io, *metadata, slottedPages);
		PropertyCollectionRootStore properties(slottedPages, *metadata);
		NativeGraphWritePlanner planner(*metadata, entities, labels, properties, indexes, graphWriteOptions());
		NativeGraphWriteTransaction seed = planner.beginTransaction();
		const uint64_t source = seed.createNode(std::array<uint64_t, 0U>{});
		const uint64_t target = seed.createNode(std::array<uint64_t, 0U>{});
		const uint64_t edge = seed.createEdge(source, target, 11U);
		ASSERT_EQ(source, 1U);
		ASSERT_EQ(edge, 1U);
		ASSERT_TRUE(seed.commit().committed());
	}

	CrashChild child(databasePath, markerPath,
					 CRASH_CHILD_COMPOUND_GRAPH_ADJACENCY_BULK_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	BTreePageStore indexes(slottedPages, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	GlobalAdjacencyIndex adjacency(indexes, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(adjacency.scan(recovered, GlobalAdjacencyDirection::GAD_OUT, 1U, 8U).edge_ids,
			  (std::vector<uint64_t>{1U}));
	EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 3U)), FormatException);
	EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 4U)), FormatException);
	recovered.release();

	NativeGraphWritePlannerOptions options = graphWriteOptions();
	options.global_adjacency_bulk_rebuild_entry_threshold = 2U;
	NativeGraphWritePlanner retryPlanner(*metadata, entities, labels, properties, indexes, options);
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t firstTarget = retry.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = retry.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = retry.createEdge(1U, firstTarget, 21U);
	const uint64_t secondEdge = retry.createEdge(1U, secondTarget, 23U);
	ASSERT_TRUE(retry.commit().committed());
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, 1U, 8U).edge_ids,
			  (std::vector<uint64_t>{1U, firstEdge, secondEdge}));
	published.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphScalarIndexCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	killCandidateAndAssertNoPartialGraphState(CRASH_CHILD_COMPOUND_GRAPH_SCALAR_INDEX_CANDIDATE_SHORT_WRITE, true);
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringCompoundGraphMetadataCandidateShortWriteRetainsNoGraphMutationAndCanRetry) {
	killCandidateAndAssertNoPartialGraphState(CRASH_CHILD_COMPOUND_GRAPH_METADATA_CANDIDATE_SHORT_WRITE, true, true);
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillAfterCompoundGraphSuperblockWriteBeforeFinalSyncRecoversOnlyACompleteGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_COMPOUND_GRAPH_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool slottedPages(io, *metadata);
	BTreePageStore indexes(slottedPages, *metadata);
	EntityLabelSetStore labels(io, *metadata, slottedPages);
	PropertyCollectionRootStore properties(slottedPages, *metadata);
	GlobalAdjacencyIndex adjacency(indexes, *metadata);

	MetadataSnapshotPin recovered = metadata->openSnapshot();
	bool graphPublished = false;
	try {
		static_cast<void>(entities.readNode(recovered, 1U));
		graphPublished = true;
	} catch (const FormatException &) {
		graphPublished = false;
	}
	if (graphPublished) {
		EXPECT_NO_THROW(static_cast<void>(entities.readNode(recovered, 2U)));
		EXPECT_NO_THROW(static_cast<void>(entities.readEdge(recovered, 1U)));
		EXPECT_EQ(adjacency.scan(recovered, GlobalAdjacencyDirection::GAD_OUT, 1U, 1U).edge_ids,
				  std::vector<uint64_t>{1U});
		EXPECT_EQ(adjacency.scan(recovered, GlobalAdjacencyDirection::GAD_IN, 2U, 1U).edge_ids,
				  std::vector<uint64_t>{1U});
		EXPECT_EQ(properties.readEntity(recovered, {EntityPropertyOwnerKind::EPOK_NODE, 1U}),
				  propertyRoot(1U, Node::typeId, 42));
		EXPECT_EQ(properties.readEntity(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, 1U}),
				  propertyRoot(1U, Edge::typeId, 84));
		BTreeIndex scalarIndex(CRASH_SCALAR_INDEX_ID, indexes, *metadata);
		const std::array<std::byte, 1U> key = {std::byte{'s'}};
		EXPECT_EQ(scalarIndex.find(recovered, key), std::optional<uint64_t>(1U));
	} else {
		EXPECT_THROW(static_cast<void>(entities.readNode(recovered, 2U)), FormatException);
		EXPECT_THROW(static_cast<void>(entities.readEdge(recovered, 1U)), FormatException);
		EXPECT_FALSE(metadata->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
		EXPECT_FALSE(metadata->findIndexRootOptional(recovered, CRASH_SCALAR_INDEX_ID).has_value());
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, 1U})
						 .has_value());
		EXPECT_FALSE(metadata->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, 1U})
						 .has_value());
	}
	recovered.release();

	if (!graphPublished) {
		NativeGraphWritePlanner retryPlanner(*metadata, entities, labels, properties, indexes, graphWriteOptions());
		NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
		const uint64_t firstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
		const uint64_t secondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
		const uint64_t edge = retry.createEdge(firstNode, secondNode, 11U);
		retry.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
		retry.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
		const std::array<BTreePageEntry, 1U> entries{{{{std::byte{'s'}}, firstNode}}};
		retry.stageScalarIndexCreate(indexes, CRASH_SCALAR_INDEX_ID, entries);
		ASSERT_TRUE(retry.commit().committed());
		MetadataSnapshotPin published = metadata->openSnapshot();
		EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
				  std::vector<uint64_t>{edge});
		EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
				  std::vector<uint64_t>{edge});
		EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
				  propertyRoot(firstNode, Node::typeId, 42));
		EXPECT_EQ(properties.readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
				  propertyRoot(edge, Edge::typeId, 84));
		BTreeIndex scalarIndex(CRASH_SCALAR_INDEX_ID, indexes, *metadata);
		const std::array<std::byte, 1U> key = {std::byte{'s'}};
		EXPECT_EQ(scalarIndex.find(published, key), std::optional<uint64_t>(firstNode));
		published.release();
	}
}

TEST_F(RelocatableProcessCrashGraphWriteTest, KillDuringEntityPageEvacuationCandidateRetainsSourceAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ENTITY_PAGE_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	NodeEntityRecord original;
	original.inline_label_ids = {7U, 9U, 11U};
	original.inline_label_count = 3U;
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(recovered, 1001U), original);
	EXPECT_EQ(metadata->findPageLocation(recovered, PageId(101U)).segment_id, SegmentId(31U));
	const EntityPageMutationResult retried =
			entities.evacuateNodePage(recovered, PageId(101U), SegmentId(32U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.page.segment_id, SegmentId(32U));
	recovered.release();
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(published, 1001U), original);
	EXPECT_EQ(metadata->findPageLocation(published, PageId(101U)).segment_id, SegmentId(32U));
	published.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringEntityLabelPageEvacuationCandidateRetainsSourceAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pages);
	std::vector<uint64_t> expected;
	expected.reserve(RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	for (uint64_t label = 7U; label < 7U + 2U * (RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U); label += 2U) {
		expected.push_back(label);
	}
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(recovered, PageId(102U)).segment_id, SegmentId(31U));
	EXPECT_EQ(labels.readNodeLabels(recovered, entities.readNode(recovered, 1001U)), expected);
	const EntityPageMutationResult retried =
			entities.evacuatePoolPage(recovered, PageId(102U), SegmentId(32U), LogSequenceNumber(5U));
	EXPECT_EQ(retried.page.segment_id, SegmentId(32U));
	recovered.release();

	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(published, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(labels.readNodeLabels(published, entities.readNode(published, 1001U)), expected);
	published.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillAfterEntityLabelPageEvacuationPublicationReopensDestinationAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ENTITY_LABEL_PAGE_EVACUATION_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pages);
	std::vector<uint64_t> expected;
	expected.reserve(RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U);
	for (uint64_t label = 7U; label < 7U + 2U * (RSS_ENTITY_INLINE_LABEL_CAPACITY + 2U); label += 2U) {
		expected.push_back(label);
	}
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(5U));
	EXPECT_EQ(metadata->findPageLocation(recovered, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(labels.readNodeLabels(recovered, entities.readNode(recovered, 1001U)), expected);
	NodeEntityRecord appended;
	appended.inline_label_ids = {13U, 15U, 17U};
	appended.inline_label_count = 3U;
	const EntityPageInsertResult inserted =
			entities.insertNode(recovered, PageId(201U), 1003U, appended, LogSequenceNumber(6U));
	EXPECT_EQ(inserted.pool.segment_id, SegmentId(32U));
	recovered.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(6U));
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(labels.readNodeLabels(current, entities.readNode(current, 1001U)), expected);
	EXPECT_EQ(entities.readNode(current, 1003U), appended);
	current.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringEntityPageReplacementCandidateShortWriteRetainsNodeAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ENTITY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	NodeEntityRecord original;
	original.inline_label_ids = {7U, 9U, 11U};
	original.inline_label_count = 3U;
	EXPECT_EQ(entities.readNode(recovered, 1001U), original);
	EXPECT_EQ(metadata->findPageLocation(recovered, PageId(101U)).page_index, 1U);
	EXPECT_EQ(metadata->findNodePageLocation(recovered, 1001U).page_id, PageId(101U));

	NodeEntityRecord replacement;
	replacement.inline_label_ids = {13U, 15U, 17U};
	replacement.inline_label_count = 3U;
	const EntityPageMutationResult retried = entities.updateNode(recovered, 1001U, replacement, LogSequenceNumber(3U));
	EXPECT_EQ(retried.page.page_index, 2U);
	recovered.release();
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(entities.readNode(published, 1001U), replacement);
	EXPECT_EQ(metadata->findPageLocation(published, PageId(101U)).page_index, 2U);
	published.release();
}

TEST_F(RelocatableProcessCrashGraphWriteTest,
	   KillDuringPropertyPageReplacementCandidateShortWriteRetainsRootAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_PROPERTY_PAGE_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	EntityPageStore entities(io, *metadata);
	SlottedPagePool pages(io, *metadata);
	PropertyCollectionRootStore properties(pages, *metadata);
	const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
	const PropertyCollectionRoot original{1001U, Node::typeId, {{3U, PropertyValue("before crash")}}};
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(recovered, key), original);
	const EntityPropertyLocation oldLocation = metadata->findEntityPropertyLocation(recovered, key);
	EXPECT_EQ(metadata->findPageLocation(recovered, oldLocation.page_id).page_index, 1U);

	const PropertyCollectionRoot replacement{1001U, Node::typeId, {{3U, PropertyValue("after crash")}}};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties.planEntityRootReplacements(recovered, std::span(&replacement, 1U), options);
	static_cast<void>(entities.applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, plans, properties, options, LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(properties.readEntity(published, key), replacement);
	const EntityPropertyLocation newLocation = metadata->findEntityPropertyLocation(published, key);
	EXPECT_EQ(newLocation.page_id, oldLocation.page_id);
	EXPECT_EQ(metadata->findPageLocation(published, newLocation.page_id).page_index, 2U);
	published.release();
}
