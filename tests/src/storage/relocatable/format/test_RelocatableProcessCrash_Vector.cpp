/**
 * @file test_RelocatableProcessCrash_Vector.cpp
 * @brief Real child-process crash coverage for native Vector publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_VECTOR_RAW_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"vector-raw-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_PQ_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"vector-pq-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"vector-manifest-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE =
			"vector-manifest-replacement-candidate";
	constexpr std::string_view CRASH_CHILD_VECTOR_MANIFEST_EVACUATION_CANDIDATE =
			"vector-manifest-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_VECTOR_RAW_EVACUATION_CANDIDATE =
			"vector-raw-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_VECTOR_PQ_EVACUATION_CANDIDATE =
			"vector-pq-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_BEFORE_PUBLICATION =
			"vector-base-rebuild-before-publication";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_SHORT_WRITE =
			"vector-base-rebuild-codebook-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_SHORT_WRITE =
			"vector-base-rebuild-pq-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_SHORT_WRITE =
			"vector-base-rebuild-adjacency-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_SHORT_WRITE =
			"vector-base-rebuild-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_DISK_FULL =
			"vector-base-rebuild-metadata-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC =
			"vector-base-rebuild-after-superblock-write-before-final-sync";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_PUBLICATION =
			"vector-base-rebuild-after-publication";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_DISK_FULL =
			"vector-base-rebuild-codebook-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_DISK_FULL =
			"vector-base-rebuild-pq-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_DISK_FULL =
			"vector-base-rebuild-adjacency-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_BEFORE_PUBLICATION =
			"vector-base-rebuild-delta-retirement-before-publication";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_AFTER_PUBLICATION =
			"vector-base-rebuild-delta-retirement-after-publication";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_DISK_FULL =
			"vector-base-rebuild-delta-retirement-metadata-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE =
			"vector-base-rebuild-delta-retirement-metadata-candidate-short-write";
	constexpr uint64_t PQ_MAPPING_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A1);
	constexpr uint64_t ADJACENCY_MAPPING_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A1);

	using graph::test::relocatable_crash::CrashChild;

	VectorManifest vectorManifest(LogSequenceNumber lsn, uint32_t rawRecordCount) {
		VectorManifest result;
		result.index_id = 17U;
		result.page_id = PageId(101U);
		result.generation = Generation(1U);
		result.committed_lsn = lsn;
		result.dimensions = 16U;
		result.entries = {{PageId(201U), VectorStorageKind::VSK_RAW, rawRecordCount},
						  {PageId(202U), VectorStorageKind::VSK_PQ, 3U}};
		return result;
	}

	VectorManifestPage vectorManifestPage(const VectorManifest &manifest) {
		return VectorManifestPage::create({manifest.page_id, manifest.index_id, PageOwnerKind::POK_VECTOR_MANIFEST, 0U,
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, manifest.generation, manifest.committed_lsn},
									 manifest);
	}

	SharedPagePoolAllocationRequest vectorManifestRequest(const VectorManifest &manifest) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(73U);
		result.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		result.pool_owner_id = manifest.index_id;
		result.generation = manifest.generation;
		result.page_id = manifest.page_id;
		result.page_owner_id = manifest.index_id;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		return result;
	}

	VectorPage vectorPage(VectorStorageKind kind, PageId pageId, Generation generation, LogSequenceNumber lsn,
					  std::byte marker) {
		VectorPageHeader header;
		header.index_id = 17U;
		header.page_id = pageId;
		header.generation = generation;
		header.kind = kind;
		header.codebook_generation = kind == VectorStorageKind::VSK_PQ ? Generation(1U) : Generation{};
		header.record_bytes = 16U;
		header.slot_capacity = 3U;
		header.bitmap_bytes = 1U;
		header.committed_lsn = lsn;
		const PageOwnerKind ownerKind =
				kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW : PageOwnerKind::POK_VECTOR_PQ;
		VectorPage result = VectorPage::create(
				{pageId, header.index_id, ownerKind, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, generation, lsn}, header);
		static_cast<void>(result.insert(std::vector<std::byte>(16U, marker)));
		return result;
	}

	SharedPagePoolAllocationRequest vectorPageRequest(VectorStorageKind kind, SegmentId segmentId, Generation generation,
																PageId pageId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = kind == VectorStorageKind::VSK_RAW ? SegmentKind::SGK_VECTOR_RAW_POOL
																	 : SegmentKind::SGK_VECTOR_PQ_POOL;
		result.pool_owner_id = 17U;
		result.generation = generation;
		result.page_id = pageId;
		result.page_owner_id = 17U;
		result.page_owner_kind =
				kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW : PageOwnerKind::POK_VECTOR_PQ;
		return result;
	}

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> result(values.size() * sizeof(uint16_t));
		size_t ordinal = 0U;
		for (const float value: values) {
			LittleEndianCodec::writeU16(result, ordinal++ * sizeof(uint16_t),
								 static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
		}
		return result;
	}

	std::vector<DeterministicPqTrainingRecord> baseRebuildRecords() {
		return {{1U, bfloat16({0.0F, 0.0F, 10.0F, 10.0F})}, {2U, bfloat16({0.0F, 1.0F, 9.0F, 10.0F})},
				{3U, bfloat16({9.0F, 10.0F, 0.0F, 0.0F})}, {4U, bfloat16({10.0F, 9.0F, 1.0F, 0.0F})},
				{5U, bfloat16({5.0F, 5.0F, 5.0F, 5.0F})}};
	}

	OfflineVectorBaseRebuildOptions baseRebuildOptions(uint64_t codebookId = 71U,
											   Generation adjacencyGeneration = Generation(1U)) {
		OfflineVectorBaseRebuildOptions result;
		result.pq.training = {4U, 2U, 2U, 3U, 16U, 5U};
		result.pq.codebook.codebook_id = codebookId;
		result.pq.codebook.manifest_segment_id = SegmentId(codebookId);
		result.pq.codebook.generation = Generation(1U);
		result.pq.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.pq.codebook.dimensions = result.pq.training.dimensions;
		result.pq.codebook.subspace_count = result.pq.training.subspace_count;
		result.pq.codebook.centroid_count = result.pq.training.centroid_count;
		result.pq.codebook_first_slot = PhysicalSlotId(50U);
		result.pq.codebook_last_slot = PhysicalSlotId(50U);
		result.pq.pages = {PhysicalSlotId(51U), PhysicalSlotId(55U), Generation(1U), 16U, 4U};
		result.pq.mapping_index_id = PQ_MAPPING_ID;
		result.pq.mapping.tree = {PhysicalSlotId(56U), PhysicalSlotId(60U), PQ_MAPPING_ID, 16U, 4U};
		result.pq.mapping.maximum_entries = 16U;
		result.adjacency.maximum_exact_records = 16U;
		result.adjacency.maximum_degree = 2U;
		result.adjacency.adjacency_generation = adjacencyGeneration;
		result.adjacency.pages = {PhysicalSlotId(61U), PhysicalSlotId(65U), 16U, 4U};
		result.adjacency.mapping_index_id = ADJACENCY_MAPPING_ID;
		result.adjacency.mapping.tree = {PhysicalSlotId(66U), PhysicalSlotId(70U), ADJACENCY_MAPPING_ID, 16U, 4U};
		result.adjacency.mapping.maximum_entries = 16U;
		result.maximum_delta_mapping_pages = 4U;
		result.maximum_delta_overlay_entries = 16U;
		return result;
	}

	RawVectorMaterializationRecord rawBaseRebuildRecord(const DeterministicPqTrainingRecord &record) {
		RawVectorMaterializationRecord result;
		result.vector_id = record.vector_id;
		result.bytes.resize(sizeof(uint64_t) + record.raw_bfloat16.size());
		LittleEndianCodec::writeU64(result.bytes, 0U, record.vector_id);
		std::copy(record.raw_bfloat16.begin(), record.raw_bfloat16.end(), result.bytes.begin() + sizeof(uint64_t));
		return result;
	}

	void materializeBaseRebuildSource(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
									 VectorManifestStore &manifests) {
		const std::vector<DeterministicPqTrainingRecord> records = baseRebuildRecords();
		std::vector<RawVectorMaterializationRecord> raw;
		raw.reserve(records.size());
		for (const DeterministicPqTrainingRecord &record: records) {
			raw.push_back(rawBaseRebuildRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(
				17U, raw, {PhysicalSlotId(40U), PhysicalSlotId(40U), 16U, 4U}, LogSequenceNumber(2U)));
		VectorManifest manifest;
		manifest.index_id = 17U;
		manifest.dimensions = 4U;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(3U)));
	}

	void overwriteManifest(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
							   const VectorManifest &manifest) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																 pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const PageOwnerHeader owner{manifest.page_id,
								manifest.index_id,
								PageOwnerKind::POK_VECTOR_MANIFEST,
								0U,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								manifest.generation,
								manifest.committed_lsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		const std::vector<std::byte> bytes = page.serialize();
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		io->sync();
	}

	VectorManifest materializeCommittedDeltaOverlay(BTreePageStore &trees, VectorPageStore &vectors,
												  VectorAdjacencyPageStore &adjacencies, VectorCodebookStore &codebooks,
												  VectorManifestStore &manifests, RelocatableMetadataStore &metadata,
												  const std::shared_ptr<StorageIO> &io) {
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
		constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
		const VectorDeltaMappingBuildOptions deltaOptions{
				{PhysicalSlotId(71U), PhysicalSlotId(75U), deltaMappingId, 16U, 4U}, 16U};
		const std::array<VectorDeltaEntry, 1U> entries{{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
		static_cast<void>(VectorDeltaMapping::build(
				trees, metadata, deltaMappingId, entries, deltaOptions,
				LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U)));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, deltaMappingId);
		snapshot.release();
		VectorManifest result = first.manifest;
		result.delta_mapping_index_id = deltaMappingId;
		result.delta_mapping_root_page_id = root.root_page_id;
		result.delta_mapping_root_generation = root.root_generation;
		result.delta_entry_count = 1U;
		result.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
		overwriteManifest(io, metadata, result);
		return result;
	}

} // namespace

namespace graph::test {

	int runRelocatableVectorCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
									  const std::filesystem::path &markerPath) {
		if (mode == CRASH_CHILD_VECTOR_RAW_EVACUATION_CANDIDATE ||
			mode == CRASH_CHILD_VECTOR_PQ_EVACUATION_CANDIDATE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 118;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 119;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				const bool pq = mode == CRASH_CHILD_VECTOR_PQ_EVACUATION_CANDIDATE;
				const VectorStorageKind kind = pq ? VectorStorageKind::VSK_PQ : VectorStorageKind::VSK_RAW;
				const SegmentId sourceSegment = pq ? SegmentId(73U) : SegmentId(71U);
				const SegmentId destinationSegment = pq ? SegmentId(74U) : SegmentId(72U);
				const PhysicalSlotId sourceSlot = pq ? PhysicalSlotId(42U) : PhysicalSlotId(40U);
				const PhysicalSlotId destinationSlot = pq ? PhysicalSlotId(43U) : PhysicalSlotId(41U);
				const PageId sourcePageId(101U);
				const auto sourceRequest = vectorPageRequest(kind, sourceSegment, Generation(1U), sourcePageId);
				if (!vectors.allocate(sourceRequest, vectorPage(kind, sourcePageId, Generation(1U),
																						LogSequenceNumber(2U), std::byte{0x11}),
												sourceSlot, sourceSlot, LogSequenceNumber(2U))
							 .has_value()) {
					return 120;
				}
				const PageId destinationPageId(102U);
				const auto destinationRequest = vectorPageRequest(kind, destinationSegment, Generation(2U), destinationPageId);
				const auto destination = vectors.allocate(destinationRequest,
						vectorPage(kind, destinationPageId, Generation(2U), LogSequenceNumber(3U), std::byte{0x22}),
						destinationSlot, destinationSlot, LogSequenceNumber(3U));
				if (!destination.has_value()) return 121;
				MetadataSnapshotPin sourceSnapshot = metadata->openSnapshot();
				const VectorPage source = vectors.read(sourceSnapshot, sourcePageId);
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						sourceSnapshot.superblock().data_region_offset, destination->pool.physical_slot) +
						static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
				sourceSnapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterDurableCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
												  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
								if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return persisted;
								}
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(vectors.evacuate(source, destinationSegment, LogSequenceNumber(4U)));
				return 122;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 123;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_RAW_REPLACEMENT_CANDIDATE_SHORT_WRITE ||
			mode == CRASH_CHILD_VECTOR_PQ_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 96;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 97;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				const bool pq = mode == CRASH_CHILD_VECTOR_PQ_REPLACEMENT_CANDIDATE_SHORT_WRITE;
				auto makePage = [pq](LogSequenceNumber lsn, std::byte marker) {
					VectorPageHeader header;
					header.index_id = 17U;
					header.page_id = PageId(101U);
					header.generation = Generation(1U);
					header.kind = pq ? VectorStorageKind::VSK_PQ : VectorStorageKind::VSK_RAW;
					header.codebook_generation = pq ? Generation(1U) : Generation{};
					header.record_bytes = 16U;
					header.slot_capacity = 3U;
					header.bitmap_bytes = 1U;
					header.committed_lsn = lsn;
					const PageOwnerHeader owner{header.page_id,
							header.index_id,
							pq ? PageOwnerKind::POK_VECTOR_PQ : PageOwnerKind::POK_VECTOR_RAW,
							0U,
							RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
							header.generation,
							lsn};
					VectorPage page = VectorPage::create(owner, header);
					static_cast<void>(page.insert(std::vector<std::byte>(16U, marker)));
					return page;
				};
				SharedPagePoolAllocationRequest request;
				request.segment_id = pq ? SegmentId(72U) : SegmentId(71U);
				request.pool_kind = pq ? SegmentKind::SGK_VECTOR_PQ_POOL : SegmentKind::SGK_VECTOR_RAW_POOL;
				request.pool_owner_id = 17U;
				request.generation = Generation(1U);
				request.page_id = PageId(101U);
				request.page_owner_id = 17U;
				request.page_owner_kind = pq ? PageOwnerKind::POK_VECTOR_PQ : PageOwnerKind::POK_VECTOR_RAW;
				const auto allocated = vectors.allocate(request, makePage(LogSequenceNumber(2U), std::byte{0x11}),
						PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U));
				if (!allocated.has_value()) return 98;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset =
						PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																					allocated->pool.physical_slot) +
						2U * RSS_PAGE_SIZE;
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																					 pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								if (portable_pwrite(fd, bytes, 16U, offset) != 16) return static_cast<pwrite_ssize_t>(-1);
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(vectors.replace(makePage(LogSequenceNumber(3U), std::byte{0x22}), request.segment_id,
						LogSequenceNumber(2U), LogSequenceNumber(3U)));
				return 99;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 100;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 101;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 102;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorManifestStore manifests(io, *metadata);
				const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
				const auto allocated = manifests.allocate(vectorManifestRequest(original), vectorManifestPage(original),
						PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U));
				if (!allocated.has_value()) return 103;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset =
						PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																					allocated->pool.physical_slot) +
						2U * RSS_PAGE_SIZE;
				snapshot.release();
				VectorManifest replacement = original;
				replacement.committed_lsn = LogSequenceNumber(3U);
				replacement.entries.front().record_count = 7U;
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																					 pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								if (portable_pwrite(fd, bytes, 16U, offset) != 16) return static_cast<pwrite_ssize_t>(-1);
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(manifests.replace(vectorManifestPage(replacement), allocated->pool.segment_id,
						LogSequenceNumber(2U), LogSequenceNumber(3U)));
				return 104;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 105;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_BEFORE_PUBLICATION ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_SHORT_WRITE ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_PUBLICATION ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_DISK_FULL ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_DISK_FULL) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 106;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 107;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				const bool beforePublication = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_BEFORE_PUBLICATION;
				const bool afterPublication = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_PUBLICATION;
				const bool shortCodebook = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_SHORT_WRITE;
				const bool diskFullCodebook = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_DISK_FULL;
				const bool diskFullPq = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_DISK_FULL;
				const bool candidateMode = shortCodebook || diskFullCodebook || diskFullPq;
				uint64_t candidateOffset = 0U;
				if (candidateMode) {
					MetadataSnapshotPin snapshot = metadata->openSnapshot();
					candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
							snapshot.superblock().data_region_offset, PhysicalSlotId(diskFullPq ? 51U : 50U));
					snapshot.release();
				}
				bool diskFull = false;
				StorageIO::ScopedNativeWriteHookForTesting interrupt(
						[candidateOffset, candidateMode, beforePublication, afterPublication, shortCodebook, diskFullCodebook, diskFullPq,
																					 &diskFull, &markerPath](file_handle_t fd, const void *bytes,
																												 size_t byteCount, pwrite_off_t offset) {
							const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
							if ((beforePublication || afterPublication) && offset >= 0 &&
								absoluteOffset < RSS_FILE_BOOTSTRAP_BYTES && byteCount == RSS_SUPERBLOCK_BYTES) {
								if (afterPublication) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
								}
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							if (candidateMode && absoluteOffset == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								if (shortCodebook) {
									if (portable_pwrite(fd, bytes, 16U, offset) != 16) return static_cast<pwrite_ssize_t>(-1);
									graph::test::relocatable_crash::writeMarker(markerPath);
									graph::test::relocatable_crash::waitToBeKilled();
								}
								if (diskFullCodebook || diskFullPq) {
									diskFull = true;
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				try {
					static_cast<void>(OfflineVectorBaseRebuild::run(trees, vectors, adjacencies, codebooks, manifests,
							*metadata, 17U, baseRebuildRecords(), baseRebuildOptions()));
				} catch (const std::system_error &) {
					if (!diskFull) throw;
				}
				if (diskFull) {
					graph::test::relocatable_crash::writeMarker(markerPath);
					graph::test::relocatable_crash::waitToBeKilled();
				}
				return 108;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 109;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 91;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 92;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorManifestStore manifests(io, *metadata);
				const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
				const auto allocated = manifests.allocate(vectorManifestRequest(original), vectorManifestPage(original),
						PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U));
				if (!allocated.has_value()) return 93;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset =
						PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																					allocated->pool.physical_slot) +
						2U * RSS_PAGE_SIZE;
				snapshot.release();
				VectorManifest replacement = original;
				replacement.committed_lsn = LogSequenceNumber(3U);
				replacement.entries.front().record_count = 7U;
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterDurableCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																					 pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return written;
								}
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(manifests.replace(vectorManifestPage(replacement), allocated->pool.segment_id,
						LogSequenceNumber(2U), LogSequenceNumber(3U)));
				return 94;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 95;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_MANIFEST_EVACUATION_CANDIDATE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 89;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 90;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorManifestStore manifests(io, *metadata);
				const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
				auto sourceRequest = vectorManifestRequest(original);
				sourceRequest.segment_id = SegmentId(71U);
				if (!manifests.allocate(sourceRequest, vectorManifestPage(original), PhysicalSlotId(40U),
										   PhysicalSlotId(40U), LogSequenceNumber(2U))
							 .has_value()) {
					return 91;
				}
				VectorManifest destination = vectorManifest(LogSequenceNumber(3U), 1U);
				destination.index_id = 18U;
				destination.page_id = PageId(102U);
				destination.generation = Generation(2U);
				auto destinationRequest = vectorManifestRequest(destination);
				destinationRequest.segment_id = SegmentId(72U);
				const auto allocatedDestination = manifests.allocate(destinationRequest, vectorManifestPage(destination),
						PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U));
				if (!allocatedDestination.has_value()) return 92;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, allocatedDestination->pool.physical_slot) +
						2U * RSS_PAGE_SIZE;
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterDurableCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																						  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(manifests.evacuate(original.index_id, SegmentId(72U), LogSequenceNumber(4U)));
				return 93;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 94;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_SHORT_WRITE ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_SHORT_WRITE ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_SHORT_WRITE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 79;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 80;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const bool metadataCandidate = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_SHORT_WRITE;
				const bool pqCandidate = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_SHORT_WRITE;
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, PhysicalSlotId(pqCandidate ? 51U : 61U));
				const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, metadataPoolOffset, metadataCandidate, &markerPath](
								file_handle_t fd, const void *bytes, size_t byteCount,
														 pwrite_off_t offset) {
							const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
							const bool reachesCandidate = metadataCandidate
												? absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
														  absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE
												: absoluteOffset == candidateOffset;
							if (reachesCandidate && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) return partial;
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(OfflineVectorBaseRebuild::run(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
						baseRebuildOptions()));
				return 81;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 82;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_DISK_FULL) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 83;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 84;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
				bool diskFull = false;
				{
					StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
							[metadataPoolOffset, &diskFull](file_handle_t fd, const void *bytes, size_t byteCount,
																pwrite_off_t offset) {
								const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
								if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
									absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
									diskFull = true;
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					try {
						static_cast<void>(OfflineVectorBaseRebuild::run(
								trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
								baseRebuildOptions()));
					} catch (const std::system_error &) {
						// The marker follows the writer's completed native disk-full cleanup path.
					}
				}
				if (!diskFull) return 85;
				graph::test::relocatable_crash::writeMarker(markerPath);
				graph::test::relocatable_crash::waitToBeKilled();
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 86;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 87;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 88;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				StorageIO::ScopedNativeWriteHookForTesting pauseBeforeFinalSync(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount)) return written;
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(OfflineVectorBaseRebuild::run(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
						baseRebuildOptions()));
				return 89;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 90;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 75;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 76;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				static_cast<void>(materializeCommittedDeltaOverlay(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, io));
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting shortMetadataCandidate(
						[metadataPoolOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																 pwrite_off_t offset) {
							const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
							if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
								absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) return partial;
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(OfflineVectorBaseRebuild::run(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
						baseRebuildOptions(71U, Generation(2U))));
				return 77;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 78;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_DISK_FULL) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 71;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 72;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				static_cast<void>(materializeCommittedDeltaOverlay(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, io));
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
				bool diskFull = false;
				{
					StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
							[metadataPoolOffset, &diskFull](file_handle_t fd, const void *bytes, size_t byteCount,
																 pwrite_off_t offset) {
								const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
								if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
									absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
									diskFull = true;
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					try {
						static_cast<void>(OfflineVectorBaseRebuild::run(
								trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
								baseRebuildOptions(71U, Generation(2U))));
					} catch (const std::system_error &) {
						// The marker below is emitted only after the metadata candidate receives disk-full.
					}
				}
				if (!diskFull) return 73;
				graph::test::relocatable_crash::writeMarker(markerPath);
				graph::test::relocatable_crash::waitToBeKilled();
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 74;
			}
		}
		if (mode == CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_BEFORE_PUBLICATION ||
			mode == CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_AFTER_PUBLICATION) {
			try {
				auto file = std::make_shared<std::fstream>(databasePath,
						std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!file->is_open()) return 67;
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) return 68;
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				VectorPageStore vectors(io, *metadata);
				VectorAdjacencyPageStore adjacencies(io, *metadata);
				VectorManifestStore manifests(io, *metadata);
				SlottedPagePool slotted(io, *metadata);
				BTreePageStore trees(slotted, *metadata);
				PhysicalSegmentAllocator segments(io, *metadata);
				VectorCodebookStore codebooks(segments, *metadata);
				materializeBaseRebuildSource(*metadata, vectors, manifests);
				static_cast<void>(materializeCommittedDeltaOverlay(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, io));
				const bool afterPublication = mode == CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_AFTER_PUBLICATION;
				StorageIO::ScopedNativeWriteHookForTesting pauseAtPublication(
						[&markerPath, afterPublication](file_handle_t fd, const void *bytes, size_t byteCount,
													  pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								if (afterPublication) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
								}
								graph::test::relocatable_crash::writeMarker(markerPath);
								graph::test::relocatable_crash::waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(OfflineVectorBaseRebuild::run(
						trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
						baseRebuildOptions(71U, Generation(2U))));
				return 69;
			} catch (const std::exception &error) {
				graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
				return 70;
			}
		}
		if (mode != CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_DISK_FULL) return -1;
		try {
			auto file = std::make_shared<std::fstream>(databasePath,
					std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			if (!file->is_open()) return 63;
			const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
			if (handle == INVALID_FILE_HANDLE) return 64;
			auto io = std::make_shared<StorageIO>(file, handle, handle);
			Superblock initial;
			initial.database_id = DatabaseId(0x1020304050607080ULL);
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
			VectorPageStore vectors(io, *metadata);
			VectorAdjacencyPageStore adjacencies(io, *metadata);
			VectorManifestStore manifests(io, *metadata);
			SlottedPagePool slotted(io, *metadata);
			BTreePageStore trees(slotted, *metadata);
			PhysicalSegmentAllocator segments(io, *metadata);
			VectorCodebookStore codebooks(segments, *metadata);
			materializeBaseRebuildSource(*metadata, vectors, manifests);
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
					snapshot.superblock().data_region_offset, PhysicalSlotId(61U));
			snapshot.release();
			bool diskFull = false;
			{
				StorageIO::ScopedNativeWriteHookForTesting failAdjacencyCandidate(
						[candidateOffset, &diskFull](file_handle_t fd, const void *bytes, size_t byteCount,
														pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								diskFull = true;
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				try {
					static_cast<void>(OfflineVectorBaseRebuild::run(trees, vectors, adjacencies, codebooks, manifests,
							*metadata, 17U, baseRebuildRecords(), baseRebuildOptions()));
				} catch (const std::system_error &) {
					// The marker below is emitted only after the native disk-full edge is reached.
				}
			}
			if (!diskFull) return 65;
			relocatable_crash::writeMarker(markerPath);
			relocatable_crash::waitToBeKilled();
		} catch (const std::exception &error) {
			relocatable_crash::writeFailureMarker(markerPath, error.what());
			return 66;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashVectorTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() / ("relocatable_process_crash_vector_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}
};

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorRawReplacementCandidateShortWriteRetainsOldPageAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_RAW_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(vectors.read(recovered, PageId(101U)).read(0), std::vector<std::byte>(16U, std::byte{0x11}));
	VectorPageHeader header;
	header.index_id = 17U;
	header.page_id = PageId(101U);
	header.generation = Generation(1U);
	header.kind = VectorStorageKind::VSK_RAW;
	header.record_bytes = 16U;
	header.slot_capacity = 3U;
	header.bitmap_bytes = 1U;
	header.committed_lsn = LogSequenceNumber(3U);
	VectorPage replacement = VectorPage::create(
			{header.page_id, header.index_id, PageOwnerKind::POK_VECTOR_RAW, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			 header.generation, header.committed_lsn}, header);
	static_cast<void>(replacement.insert(std::vector<std::byte>(16U, std::byte{0x22})));
	EXPECT_EQ(vectors.replace(replacement, SegmentId(71U), LogSequenceNumber(2U), LogSequenceNumber(3U)).location.page_index,
			  2U);
	recovered.release();
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(vectors.read(published, PageId(101U)).read(0), std::vector<std::byte>(16U, std::byte{0x22}));
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorPqReplacementCandidateShortWriteRetainsOldPageAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_PQ_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorPage old = vectors.read(recovered, PageId(101U));
	EXPECT_EQ(old.header().kind, VectorStorageKind::VSK_PQ);
	EXPECT_EQ(old.header().codebook_generation, Generation(1U));
	EXPECT_EQ(old.read(0), std::vector<std::byte>(16U, std::byte{0x11}));
	VectorPageHeader header = old.header();
	header.live_count = 0U;
	header.committed_lsn = LogSequenceNumber(3U);
	VectorPage replacement = VectorPage::create(
			{header.page_id, header.index_id, PageOwnerKind::POK_VECTOR_PQ, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			 header.generation, header.committed_lsn}, header);
	static_cast<void>(replacement.insert(std::vector<std::byte>(16U, std::byte{0x22})));
	EXPECT_EQ(vectors.replace(replacement, SegmentId(72U), LogSequenceNumber(2U), LogSequenceNumber(3U)).location.page_index,
			  2U);
	recovered.release();
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(vectors.read(published, PageId(101U)).read(0), std::vector<std::byte>(16U, std::byte{0x22}));
}

TEST_F(RelocatableProcessCrashVectorTest,
		KillAfterDurableRawAndPqEvacuationCandidatesRetainsPublishedPagesAndCanRetry) {
	struct EvacuationCase {
		std::string_view mode;
		VectorStorageKind kind;
		SegmentId source_segment;
		SegmentId destination_segment;
	};
	const std::array<EvacuationCase, 2U> cases{{
			{CRASH_CHILD_VECTOR_RAW_EVACUATION_CANDIDATE, VectorStorageKind::VSK_RAW, SegmentId(71U), SegmentId(72U)},
			{CRASH_CHILD_VECTOR_PQ_EVACUATION_CANDIDATE, VectorStorageKind::VSK_PQ, SegmentId(73U), SegmentId(74U)},
	}};
	for (const EvacuationCase &testCase: cases) {
		SCOPED_TRACE(std::string(testCase.mode));
		std::error_code error;
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
		CrashChild child(databasePath, markerPath, testCase.mode);
		ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
		child.terminate();

		auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		VectorPageStore vectors(io, *metadata);
		MetadataSnapshotPin recovered = metadata->openSnapshot();
		const VectorPage source = vectors.read(recovered, PageId(101U));
		EXPECT_EQ(source.header().kind, testCase.kind);
		EXPECT_EQ(source.header().generation, Generation(1U));
		EXPECT_EQ(source.read(0), std::vector<std::byte>(16U, std::byte{0x11}));
		EXPECT_EQ(metadata->findPageLocation(recovered, PageId(101U)).segment_id, testCase.source_segment);
		recovered.release();

		const SharedPagePoolAllocation retried = vectors.evacuate(source, testCase.destination_segment, LogSequenceNumber(4U));
		EXPECT_EQ(retried.location.segment_id, testCase.destination_segment);
		EXPECT_EQ(retried.location.generation, Generation(2U));
		MetadataSnapshotPin published = metadata->openSnapshot();
		EXPECT_EQ(vectors.read(published, PageId(101U)).header().generation, Generation(2U));
		EXPECT_EQ(vectors.read(published, PageId(101U)).read(0), std::vector<std::byte>(16U, std::byte{0x11}));
		EXPECT_EQ(metadata->findPageLocation(published, PageId(101U)).segment_id, testCase.destination_segment);
		EXPECT_EQ(metadata->findPageLocation(published, PageId(101U)).page_index,
				  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
		published.release();

		auto reopenedFile = std::make_shared<std::fstream>(databasePath,
				std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(reopenedFile->is_open());
		auto reopenedIo = std::make_shared<StorageIO>(reopenedFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto reopenedMetadata = RelocatableMetadataStore::open(reopenedIo);
		VectorPageStore reopenedVectors(reopenedIo, *reopenedMetadata);
		MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedVectors.read(reopened, PageId(101U)).header().generation, Generation(2U));
		EXPECT_EQ(reopenedMetadata->findPageLocation(reopened, PageId(101U)).segment_id, testCase.destination_segment);
	}
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorManifestReplacementCandidateShortWriteRetainsBothDirectoriesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *metadata);
	const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
	const VectorManifest replacement = vectorManifest(LogSequenceNumber(3U), 7U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(recovered, original.index_id).manifest(), original);
	EXPECT_EQ(metadata->findVectorManifest(recovered, original.index_id),
			  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).segment_id, SegmentId(73U));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	recovered.release();
	EXPECT_EQ(manifests.replace(vectorManifestPage(replacement), SegmentId(73U), LogSequenceNumber(2U),
							LogSequenceNumber(3U))
				  .location.page_index,
			  2U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, original.index_id).manifest(), replacement);
	EXPECT_EQ(metadata->findVectorManifest(published, original.index_id),
			  (VectorManifestLocation{replacement.index_id, replacement.page_id, replacement.generation}));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).segment_id, SegmentId(73U));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).page_index, 2U);
}

TEST_F(RelocatableProcessCrashVectorTest, KillAfterDurableVectorManifestCandidateRetainsBothDirectoriesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_MANIFEST_REPLACEMENT_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *metadata);
	const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
	const VectorManifest replacement = vectorManifest(LogSequenceNumber(3U), 7U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(recovered, original.index_id).manifest(), original);
	EXPECT_EQ(metadata->findVectorManifest(recovered, original.index_id),
			  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).segment_id, SegmentId(73U));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	recovered.release();
	EXPECT_EQ(manifests.replace(vectorManifestPage(replacement), SegmentId(73U), LogSequenceNumber(2U),
							LogSequenceNumber(3U))
				  .location.page_index,
			  2U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, original.index_id).manifest(), replacement);
	EXPECT_EQ(metadata->findVectorManifest(published, original.index_id),
			  (VectorManifestLocation{replacement.index_id, replacement.page_id, replacement.generation}));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).segment_id, SegmentId(73U));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).page_index, 2U);
}

TEST_F(RelocatableProcessCrashVectorTest,
	   KillAfterDurableVectorManifestEvacuationCandidateRetainsBothDirectoriesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_MANIFEST_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *metadata);
	const VectorManifest original = vectorManifest(LogSequenceNumber(2U), 3U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(recovered, original.index_id).manifest(), original);
	EXPECT_EQ(metadata->findVectorManifest(recovered, original.index_id),
			  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).segment_id, SegmentId(71U));
	EXPECT_EQ(metadata->findPageLocation(recovered, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	recovered.release();

	const SharedPagePoolAllocation retried = manifests.evacuate(original.index_id, SegmentId(72U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.segment_id, SegmentId(72U));
	EXPECT_EQ(retried.location.generation, Generation(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	VectorManifest expected = original;
	expected.generation = Generation(2U);
	expected.committed_lsn = LogSequenceNumber(4U);
	EXPECT_EQ(manifests.read(published, original.index_id).manifest(), expected);
	EXPECT_EQ(metadata->findVectorManifest(published, original.index_id),
			  (VectorManifestLocation{original.index_id, original.page_id, Generation(2U)}));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).segment_id, SegmentId(72U));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	published.release();
}

TEST_F(RelocatableProcessCrashVectorTest, KillBeforeVectorBaseRebuildPublicationRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	const VectorManifest current = manifests.read(published, 17U).manifest();
	EXPECT_EQ(current, retry.manifest);
	EXPECT_EQ(current.codebook_id, 71U);
	EXPECT_TRUE(current.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(current.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, current.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  current.adjacency_mapping_root_page_id);
	EXPECT_TRUE(
			metadata->findExtentLocationOptional(published, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorBaseRebuildCodebookCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorBaseRebuildPqCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorBaseRebuildAdjacencyCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, KillDuringVectorBaseRebuildMetadataCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, VectorBaseRebuildMetadataCandidateDiskFullRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_METADATA_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, KillAfterSuperblockWriteBeforeFinalSyncRecoversOnlyCompleteGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest recoveredManifest = manifests.read(recovered, 17U).manifest();
	const bool recoveredNewGeneration = recoveredManifest.codebook_id == 71U;
	EXPECT_TRUE(recoveredManifest.codebook_id == 0U || recoveredNewGeneration);
	if (recoveredNewGeneration) {
		EXPECT_TRUE(recoveredManifest.pq_mapping_root_page_id.isValid());
		EXPECT_TRUE(recoveredManifest.adjacency_mapping_root_page_id.isValid());
		EXPECT_EQ(metadata->findIndexRoot(recovered, PQ_MAPPING_ID).root_page_id,
				  recoveredManifest.pq_mapping_root_page_id);
		EXPECT_EQ(metadata->findIndexRoot(recovered, ADJACENCY_MAPPING_ID).root_page_id,
				  recoveredManifest.adjacency_mapping_root_page_id);
		EXPECT_TRUE(metadata->findExtentLocationOptional(
				recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	} else {
		EXPECT_FALSE(recoveredManifest.pq_mapping_root_page_id.isValid());
		EXPECT_FALSE(recoveredManifest.adjacency_mapping_root_page_id.isValid());
		EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
		EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	}
	recovered.release();

	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(retry.already_published, recoveredNewGeneration);
	EXPECT_TRUE(retry.manifest.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(retry.manifest.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableProcessCrashVectorTest, KillAfterVectorBaseRebuildPublicationRetainsCompleteNewGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *metadata);
	MetadataSnapshotPin published = metadata->openSnapshot();
	const VectorManifest current = manifests.read(published, 17U).manifest();
	EXPECT_EQ(current.codebook_id, 71U);
	EXPECT_TRUE(current.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(current.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, current.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  current.adjacency_mapping_root_page_id);
	EXPECT_TRUE(
			metadata->findExtentLocationOptional(published, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
}

TEST_F(RelocatableProcessCrashVectorTest, VectorBaseRebuildCodebookCandidateDiskFullRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_CODEBOOK_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, VectorBaseRebuildPqCandidateDiskFullRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_PQ_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest, VectorBaseRebuildAdjacencyCandidateDiskFullRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_ADJACENCY_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.codebook_id, 0U);
	EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(
			metadata->findExtentLocationOptional(recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(), baseRebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest,
	   KillBeforeDeltaRetirementPublicationRetainsCommittedOverlayAndCanRetryRebuild) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.delta_mapping_index_id, deltaMappingId);
	EXPECT_TRUE(oldManifest.delta_mapping_root_page_id.isValid());
	EXPECT_TRUE(oldManifest.delta_mapping_root_generation.isValid());
	EXPECT_EQ(oldManifest.delta_entry_count, 1U);
	EXPECT_EQ(metadata->findIndexRoot(recovered, deltaMappingId).root_page_id, oldManifest.delta_mapping_root_page_id);
	EXPECT_EQ(VectorDeltaMapping::findOperation(recovered, trees, *metadata, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
			baseRebuildOptions(71U, Generation(2U)));
	EXPECT_FALSE(retry.already_published);
	EXPECT_EQ(retry.manifest.delta_mapping_index_id, 0U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_FALSE(metadata->findIndexRootOptional(published, deltaMappingId).has_value());
}

TEST_F(RelocatableProcessCrashVectorTest, KillAfterDeltaRetirementPublicationRetainsCompleteReplacementGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *metadata);
	constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	MetadataSnapshotPin published = metadata->openSnapshot();
	const VectorManifest replacement = manifests.read(published, 17U).manifest();
	EXPECT_EQ(replacement.codebook_id, 71U);
	EXPECT_EQ(replacement.adjacency_generation, Generation(2U));
	EXPECT_TRUE(replacement.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(replacement.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(replacement.delta_mapping_index_id, 0U);
	EXPECT_FALSE(replacement.delta_mapping_root_page_id.isValid());
	EXPECT_FALSE(replacement.delta_mapping_root_generation.isValid());
	EXPECT_EQ(replacement.delta_entry_count, 0U);
	EXPECT_FALSE(metadata->findIndexRootOptional(published, deltaMappingId).has_value());
	EXPECT_EQ(metadata->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, replacement.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  replacement.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableProcessCrashVectorTest,
	   DeltaRetirementMetadataCandidateDiskFullRetainsCommittedOverlayAndCanRetryRebuild) {
	CrashChild child(databasePath, markerPath,
				 CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.delta_mapping_index_id, deltaMappingId);
	EXPECT_TRUE(oldManifest.delta_mapping_root_page_id.isValid());
	EXPECT_TRUE(oldManifest.delta_mapping_root_generation.isValid());
	EXPECT_EQ(oldManifest.delta_entry_count, 1U);
	EXPECT_EQ(metadata->findIndexRoot(recovered, deltaMappingId).root_page_id, oldManifest.delta_mapping_root_page_id);
	EXPECT_EQ(VectorDeltaMapping::findOperation(recovered, trees, *metadata, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
			baseRebuildOptions(71U, Generation(2U)));
	EXPECT_FALSE(retry.already_published);
	EXPECT_EQ(retry.manifest.delta_mapping_index_id, 0U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_FALSE(metadata->findIndexRootOptional(published, deltaMappingId).has_value());
}

TEST_F(RelocatableProcessCrashVectorTest,
	   KillDuringDeltaRetirementMetadataCandidateShortWriteRetainsCommittedOverlayAndCanRetryRebuild) {
	CrashChild child(databasePath, markerPath,
				 CRASH_CHILD_VECTOR_BASE_REBUILD_DELTA_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	constexpr uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(oldManifest.delta_mapping_index_id, deltaMappingId);
	EXPECT_TRUE(oldManifest.delta_mapping_root_page_id.isValid());
	EXPECT_TRUE(oldManifest.delta_mapping_root_generation.isValid());
	EXPECT_EQ(oldManifest.delta_entry_count, 1U);
	EXPECT_EQ(metadata->findIndexRoot(recovered, deltaMappingId).root_page_id, oldManifest.delta_mapping_root_page_id);
	EXPECT_EQ(VectorDeltaMapping::findOperation(recovered, trees, *metadata, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, baseRebuildRecords(),
			baseRebuildOptions(71U, Generation(2U)));
	EXPECT_FALSE(retry.already_published);
	EXPECT_EQ(retry.manifest.delta_mapping_index_id, 0U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_FALSE(metadata->findIndexRootOptional(published, deltaMappingId).has_value());
}
