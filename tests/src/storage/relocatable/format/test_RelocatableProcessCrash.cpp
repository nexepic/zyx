/**
 * @file test_RelocatableProcessCrash.cpp
 * @brief Real child-process kill and reopen coverage for A/B Superblock publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "TestProcessSupport.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace graph::storage;

namespace graph::test {

	int runRelocatableCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
								 const std::filesystem::path &markerPath);
	int runRelocatableVectorCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
									   const std::filesystem::path &markerPath);
	int runRelocatableLargeValueCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
										 const std::filesystem::path &markerPath);
	int runRelocatableBTreeCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
									  const std::filesystem::path &markerPath);
	int runRelocatableGraphWriteCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																   const std::filesystem::path &markerPath);
	int runRelocatableSharedPagePoolCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																		const std::filesystem::path &markerPath);
	int runRelocatableBulkEdgeStagingCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
												const std::filesystem::path &markerPath);
	int runRelocatableOrdinaryEdgeCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																		 const std::filesystem::path &markerPath);
	int runRelocatableAdjacencyMaintenanceCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																			 const std::filesystem::path &markerPath);
	int runRelocatableLargeValueMaintenanceCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																			  const std::filesystem::path &markerPath);
	int runRelocatableDdlCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
													 const std::filesystem::path &markerPath);
	int runDriverAbiIngestProcessCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
															 const std::filesystem::path &markerPath);

} // namespace graph::test

namespace {

	constexpr std::string_view CRASH_CHILD_BEFORE_PUBLICATION = "before-publication";
	constexpr std::string_view CRASH_CHILD_AFTER_PUBLICATION = "after-publication";
	constexpr std::string_view CRASH_CHILD_METADATA_POOL_CONTROL_SHORT_WRITE = "metadata-pool-control-short-write";
	constexpr std::string_view CRASH_CHILD_SEGMENT_CANDIDATE_SHORT_WRITE = "segment-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_PUBLICATION =
			"segment-allocation-after-publication";
	constexpr std::string_view CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_DATA_FENCE =
			"segment-allocation-after-data-fence";
	constexpr std::string_view CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_DATA_FENCE =
			"allocator-directory-after-data-fence";
	constexpr std::string_view CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_PUBLICATION =
			"allocator-directory-after-publication";
	constexpr std::string_view CRASH_CHILD_SEGMENT_RELOCATION_CANDIDATE_SHORT_WRITE =
			"segment-relocation-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_RETIRED_SEGMENT_RELEASE_CANDIDATE_SHORT_WRITE =
			"retired-segment-release-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION =
			"retired-segment-release-after-publication";
	constexpr std::string_view CRASH_CHILD_RETIRE_QUEUE_RECLAIM_CANDIDATE_SHORT_WRITE =
			"retire-queue-reclaim-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_RETIRE_QUEUE_RECLAIM_AFTER_PUBLICATION =
			"retire-queue-reclaim-after-publication";
	constexpr std::string_view CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION =
			"relocated-source-release-after-publication";
	constexpr std::string_view CRASH_CHILD_TAIL_TRUNCATE_PRE_TRUNCATE_FENCE =
			"tail-truncate-pre-truncate-fence";
	constexpr std::string_view CRASH_CHILD_TAIL_TRUNCATE_POST_TRUNCATE_FENCE = "tail-truncate-post-truncate-fence";
	constexpr std::string_view CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CANDIDATE_SHORT_WRITE =
			"emergency-reserve-rotation-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CLEANUP = "emergency-reserve-rotation-cleanup";
	constexpr std::string_view CRASH_CHILD_METADATA_PAGE_CANDIDATE_SHORT_WRITE = "metadata-page-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_METADATA_POOL_CANDIDATE_SHORT_WRITE = "metadata-pool-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_METADATA_POOL_RECLAIM_AFTER_PUBLICATION =
			"metadata-pool-reclaim-after-publication";
	constexpr std::string_view CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_CANDIDATE =
			"routed-metadata-pool-relocation-candidate";
	constexpr std::string_view CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_AFTER_PUBLICATION =
			"routed-metadata-pool-relocation-after-publication";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_MIGRATION_BEFORE_PUBLICATION =
			"bootstrap-migration-before-publication";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION =
			"bootstrap-migration-after-publication";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE = "bootstrap-migration-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM = "bootstrap-route-reclaim";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE =
			"bootstrap-post-migration-route-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE =
			"bootstrap-post-migration-header-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE =
			"bootstrap-post-migration-route-radix-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE =
			"bootstrap-post-migration-catalog-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE =
			"bootstrap-post-migration-allocator-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE =
			"bootstrap-post-migration-allocator-internal-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE =
			"bootstrap-post-migration-location-internal-candidate";
	constexpr std::string_view CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE =
			"bootstrap-post-migration-location-candidate";
	Superblock superblock(uint64_t generation, uint64_t checkpointLsn, uint64_t rootSlot) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = {PhysicalSlotId(rootSlot), 1, Generation(generation)};
		result.page_directory_root = {PhysicalSlotId(rootSlot + 1U), 2, Generation(generation)};
		result.extent_directory_root = {PhysicalSlotId(rootSlot + 2U), 3, Generation(generation)};
		result.allocator_root = {PhysicalSlotId(rootSlot + 3U), 4, Generation(generation)};
		return result;
	}

	std::vector<PageLocation> locationsThatRequireRouteRadix() {
		// One page per location-leaf range forces more route leaves than one
		// route page can hold without aliasing thousands of logical pages to a
		// single physical frame.
		const uint64_t count = static_cast<uint64_t>(RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES) + 1U;
		std::vector<PageLocation> locations;
		locations.reserve(static_cast<size_t>(count));
		for (uint64_t i = 0; i < count; ++i) {
			locations.push_back(
					{PageId(1U + i * RSS_LOCATION_LEAF_MAX_ENTRIES),
					 SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + i / RSS_PAGE_POOL_DATA_FRAME_COUNT),
					 static_cast<uint8_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + i % RSS_PAGE_POOL_DATA_FRAME_COUNT),
					 Generation(1U), 0U});
		}
		return locations;
	}

	void prepareDistinctRoutePages(StorageIO &io, uint64_t dataOffset, std::span<const CatalogEntry> pools) {
		const auto locations = locationsThatRequireRouteRadix();
		for (size_t poolIndex = 0; poolIndex < pools.size(); ++poolIndex) {
			const auto &pool = pools[poolIndex];
			const size_t begin = poolIndex * RSS_PAGE_POOL_DATA_FRAME_COUNT;
			const size_t count = (std::min)(size_t{RSS_PAGE_POOL_DATA_FRAME_COUNT}, locations.size() - begin);
			const uint32_t mask = (uint32_t{1} << count) - 1U;
			const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(dataOffset, pool.physical_slot);
			std::array<std::byte, RSS_PAGE_SIZE> control{};
			SegmentEnvelope envelope; envelope.header_bytes = RSS_PAGE_SIZE; envelope.kind = pool.kind;
			envelope.state = pool.state; envelope.segment_id = pool.segment_id; envelope.owner_id = pool.owner_id;
			envelope.generation = pool.generation; envelope.committed_lsn = pool.committed_lsn;
			const auto envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
			std::copy(envelopeBytes.begin(), envelopeBytes.end(), control.begin());
			const auto active = SharedPagePoolHeaderCodec::encode({pool.segment_id, mask, pool.generation, pool.committed_lsn, 1U});
			const auto inactive = SharedPagePoolHeaderCodec::encode({pool.segment_id, mask, pool.generation, pool.committed_lsn, 0U});
			std::copy(active.begin(), active.end(), control.begin() + RSS_ENVELOPE_BYTES);
			std::copy(inactive.begin(), inactive.end(), control.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES);
			for (size_t j = 0; j < count; ++j) {
				const auto &location = locations[begin + j];
				PageOwnerHeader owner{location.page_id, pool.owner_id, PageOwnerKind::POK_ENTITY, 0U,
					RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, pool.committed_lsn};
				const auto payload = EntityPage::create(owner, EntityPageRecordKind::EPRK_NODE).serialize();
				const auto header = PageOwnerHeaderCodec::encode(owner);
				std::array<std::byte, RSS_PAGE_SIZE> frame{};
				std::copy(header.begin(), header.end(), frame.begin());
				std::copy(payload.begin(), payload.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
				io.writeAt(offset + uint64_t(location.page_index) * RSS_PAGE_SIZE, frame.data(), frame.size());
			}
			io.writeAt(offset, control.data(), control.size());
			const std::byte zero{};
			io.writeAt(offset + RSS_SEGMENT_SIZE - 1U, &zero, 1U);
		}
	}


    void prepareEmptyCrashPool(StorageIO &io, uint64_t dataOffset, const CatalogEntry &pool) {
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(dataOffset, pool.physical_slot);
        std::array<std::byte, RSS_PAGE_SIZE> control{};
        SegmentEnvelope envelope; envelope.header_bytes = RSS_PAGE_SIZE; envelope.kind = pool.kind;
        envelope.state = pool.state; envelope.segment_id = pool.segment_id; envelope.owner_id = pool.owner_id;
        envelope.generation = pool.generation; envelope.committed_lsn = pool.committed_lsn;
        const auto encoded = SegmentEnvelopeCodec::encode(envelope);
        std::copy(encoded.begin(), encoded.end(), control.begin());
        const auto active = SharedPagePoolHeaderCodec::encode({pool.segment_id, 0U, pool.generation, pool.committed_lsn, 1U});
        const auto inactive = SharedPagePoolHeaderCodec::encode({pool.segment_id, 0U, pool.generation, pool.committed_lsn, 0U});
        std::copy(active.begin(), active.end(), control.begin() + RSS_ENVELOPE_BYTES);
        std::copy(inactive.begin(), inactive.end(), control.begin() + RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES);
        io.writeAt(offset, control.data(), control.size());
        const std::byte zero{}; io.writeAt(offset + RSS_SEGMENT_SIZE - 1U, &zero, 1U);
    }

	using CrashChild = graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
								 const std::filesystem::path &markerPath) {
		try {
			if (const int vectorResult = runRelocatableVectorCrashChild(mode, databasePath, markerPath);
				vectorResult != -1) {
				return vectorResult;
			}
			if (const int largeValueResult = runRelocatableLargeValueCrashChild(mode, databasePath, markerPath);
				largeValueResult != -1) {
				return largeValueResult;
			}
			if (const int btreeResult = runRelocatableBTreeCrashChild(mode, databasePath, markerPath);
				btreeResult != -1) {
				return btreeResult;
			}
			if (const int graphWriteResult = runRelocatableGraphWriteCrashChild(mode, databasePath, markerPath);
				graphWriteResult != -1) {
				return graphWriteResult;
			}
			if (const int sharedPagePoolResult = runRelocatableSharedPagePoolCrashChild(mode, databasePath, markerPath);
				sharedPagePoolResult != -1) {
				return sharedPagePoolResult;
			}
			if (const int bulkEdgeResult = runRelocatableBulkEdgeStagingCrashChild(mode, databasePath, markerPath);
				bulkEdgeResult != -1) {
				return bulkEdgeResult;
			}
			if (const int ordinaryEdgeResult = runRelocatableOrdinaryEdgeCrashChild(mode, databasePath, markerPath);
				ordinaryEdgeResult != -1) {
				return ordinaryEdgeResult;
			}
			if (const int adjacencyMaintenanceResult =
						runRelocatableAdjacencyMaintenanceCrashChild(mode, databasePath, markerPath);
				adjacencyMaintenanceResult != -1) {
				return adjacencyMaintenanceResult;
			}
			if (const int largeValueMaintenanceResult =
						runRelocatableLargeValueMaintenanceCrashChild(mode, databasePath, markerPath);
				largeValueMaintenanceResult != -1) {
				return largeValueMaintenanceResult;
			}
			if (const int ddlResult = runRelocatableDdlCrashChild(mode, databasePath, markerPath); ddlResult != -1) {
				return ddlResult;
			}
			if (const int driverAbiIngestResult = runDriverAbiIngestProcessCrashChild(mode, databasePath, markerPath);
				driverAbiIngestResult != -1) {
				return driverAbiIngestResult;
			}
			if (mode != CRASH_CHILD_BEFORE_PUBLICATION && mode != CRASH_CHILD_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_METADATA_POOL_CONTROL_SHORT_WRITE &&
				mode != CRASH_CHILD_SEGMENT_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_DATA_FENCE &&
				mode != CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_DATA_FENCE &&
				mode != CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_SEGMENT_RELOCATION_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_RETIRED_SEGMENT_RELEASE_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_RETIRE_QUEUE_RECLAIM_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_RETIRE_QUEUE_RECLAIM_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_TAIL_TRUNCATE_PRE_TRUNCATE_FENCE &&
				mode != CRASH_CHILD_TAIL_TRUNCATE_POST_TRUNCATE_FENCE &&
				mode != CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CLEANUP &&
				mode != CRASH_CHILD_METADATA_PAGE_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_METADATA_POOL_CANDIDATE_SHORT_WRITE &&
				mode != CRASH_CHILD_METADATA_POOL_RECLAIM_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_CANDIDATE &&
				mode != CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_BOOTSTRAP_MIGRATION_BEFORE_PUBLICATION &&
				mode != CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION &&
				mode != CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE && mode != CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE &&
				mode != CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE) {
				return 2;
			}
			auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary |
																			 std::ios::trunc);
			if (!file->is_open()) {
				return 3;
			}
			if (mode == CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_DATA_FENCE ||
				mode == CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_PUBLICATION) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 73;
				}
				auto nativeIo = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				SuperblockFile bootstrap(nativeIo);
				bootstrap.initializeNew(initial);
				MetadataPageFile pages(nativeIo, initial);
				MetadataPagePoolAllocator allocator(nativeIo, initial);
				AllocatorDirectory directory(pages, allocator);
				if (mode == CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_DATA_FENCE) {
					std::atomic<uint32_t> syncCalls{0U};
					StorageIO::ScopedNativeSyncHookForTesting pauseAfterDataFence(
							[&syncCalls, &markerPath](file_handle_t fd) {
								if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
									if (portable_fsync(fd) != 0) {
										return -1;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_fsync(fd);
							});
					static_cast<void>(directory.setAllocatedAndPublish(
							bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U)));
					return 74;
				}
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterPublication(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return written;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(directory.setAllocatedAndPublish(
						bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U)));
				return 75;
			}
			if (mode == CRASH_CHILD_METADATA_POOL_CONTROL_SHORT_WRITE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 9;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				SuperblockFile(io).initializeNew(initial);
				MetadataPagePoolAllocator allocator(io, initial);
				const PhysicalPageRef first = allocator.allocatePage(Generation(1U));
				if (!first.isValid()) {
					return 10;
				}
				io->sync();

				const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES;
				std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
				if (io->readAt(poolOffset, controlPage.data(), controlPage.size()) != controlPage.size()) {
					return 11;
				}
				const MetadataPagePoolHeader firstHeader = MetadataPagePoolHeaderCodec::decode(
						std::span<const std::byte>(controlPage)
								.subspan(RSS_ENVELOPE_BYTES, RSS_METADATA_PAGE_POOL_HEADER_BYTES));
				const MetadataPagePoolHeader secondHeader = MetadataPagePoolHeaderCodec::decode(
						std::span<const std::byte>(controlPage)
								.subspan(RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES,
										 RSS_METADATA_PAGE_POOL_HEADER_BYTES));
				const uint64_t inactiveControlOffset =
						poolOffset + (firstHeader.control_sequence >= secondHeader.control_sequence
											  ? RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES
											  : RSS_ENVELOPE_BYTES);
				bool interrupted = false;
				{
					StorageIO::ScopedNativeWriteHookForTesting shortControl(
							[inactiveControlOffset, &interrupted](file_handle_t fd, const void *bytes, size_t byteCount,
																  pwrite_off_t offset) {
								if (static_cast<uint64_t>(offset) == inactiveControlOffset &&
									byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
									interrupted = true;
									return portable_pwrite(fd, bytes, 16U, offset);
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					try {
						static_cast<void>(allocator.allocatePage(Generation(1U)));
					} catch (const std::system_error &) {
						// The marker is written only after the inactive record was actually torn.
					}
				}
				if (!interrupted) {
					return 12;
				}
				writeMarker(markerPath);
				waitToBeKilled();
			}
			if (mode == CRASH_CHILD_SEGMENT_CANDIDATE_SHORT_WRITE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 13;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
				snapshot.release();
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				request.payload = {std::byte{0xB1}, std::byte{0xB2}};
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
													   pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) {
									return partial;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(
						allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
				return 14;
			}
			if (mode == CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_PUBLICATION) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 15;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				request.payload = {std::byte{0xB3}, std::byte{0xC4}};
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterPublication(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
								if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return persisted;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
																				  LogSequenceNumber(2U)));
				return 16;
			}
			if (mode == CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_DATA_FENCE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 17;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				request.payload = {std::byte{0xA3}, std::byte{0xB4}};
				std::atomic<uint32_t> syncCalls{0U};
				StorageIO::ScopedNativeSyncHookForTesting pauseAfterDataFence(
						[&syncCalls, &markerPath](file_handle_t fd) {
							if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
								if (portable_fsync(fd) != 0) {
									return -1;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_fsync(fd);
						});
				static_cast<void>(allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
																				  LogSequenceNumber(2U)));
				return 18;
			}
			if (mode == CRASH_CHILD_METADATA_PAGE_CANDIDATE_SHORT_WRITE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 15;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
				snapshot.release();
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				request.payload = {std::byte{0xD1}, std::byte{0xD2}};
				StorageIO::ScopedNativeWriteHookForTesting shortMetadataCandidate(
						[metadataPoolOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
														  pwrite_off_t offset) {
							const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
							if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
								absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) {
									return partial;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(
						allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
				return 16;
			}
			if (mode == CRASH_CHILD_SEGMENT_RELOCATION_CANDIDATE_SHORT_WRITE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 17;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				request.payload = {std::byte{0xF1}, std::byte{0xF2}};
				if (!allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
							 .has_value()) {
					return 18;
				}
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t destinationOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, PhysicalSlotId(41U));
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting shortDestination(
						[destinationOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
														 pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == destinationOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) {
									return partial;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(allocator.relocate(request.segment_id, request.generation, PhysicalSlotId(41U),
													 PhysicalSlotId(41U), LogSequenceNumber(3U)));
				return 19;
			}
			if (mode == CRASH_CHILD_RETIRED_SEGMENT_RELEASE_CANDIDATE_SHORT_WRITE ||
				mode == CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION ||
				mode == CRASH_CHILD_RETIRE_QUEUE_RECLAIM_CANDIDATE_SHORT_WRITE ||
				mode == CRASH_CHILD_RETIRE_QUEUE_RECLAIM_AFTER_PUBLICATION ||
				mode == CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 20;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				if (mode == CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION) {
					request.payload = {std::byte{0xB1}, std::byte{0xB2}};
				}
				if (!allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
							 .has_value()) {
					return 21;
				}
				if (mode == CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION) {
					if (!allocator.relocate(request.segment_id, request.generation, PhysicalSlotId(41U), PhysicalSlotId(41U),
																					 LogSequenceNumber(4U))
													.has_value()) {
						return 25;
					}
				} else {
					static_cast<void>(allocator.retire(request.segment_id, request.generation, LogSequenceNumber(3U)));
				}
				if (mode == CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION ||
					mode == CRASH_CHILD_RETIRE_QUEUE_RECLAIM_AFTER_PUBLICATION ||
					mode == CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION) {
					StorageIO::ScopedNativeWriteHookForTesting pauseAfterPublication(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_SUPERBLOCK_BYTES) {
									const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
									if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return persisted;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					if (mode == CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION) {
						static_cast<void>(allocator.releaseRetired(request.segment_id, request.generation,
																					  LogSequenceNumber(4U)));
					} else if (mode == CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION) {
						static_cast<void>(allocator.releaseRelocatedSource(request.segment_id, Generation(2U),
																											 LogSequenceNumber(5U)));
					} else {
						static_cast<void>(metadata->reclaimRetiredPhysicalSlots(LogSequenceNumber(4U)));
					}
					return 24;
				}
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
								if (partial != 16) {
									return partial;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				if (mode == CRASH_CHILD_RETIRED_SEGMENT_RELEASE_CANDIDATE_SHORT_WRITE) {
					static_cast<void>(
							allocator.releaseRetired(request.segment_id, request.generation, LogSequenceNumber(4U)));
					return 22;
				}
				static_cast<void>(metadata->reclaimRetiredPhysicalSlots(LogSequenceNumber(4U)));
				return 23;
			}
			if (mode == CRASH_CHILD_METADATA_POOL_RECLAIM_AFTER_PUBLICATION) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 25;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNewComplete(io, initial);
                std::vector<CatalogSlotReservationRequest> requests;
                for (uint64_t ordinal = 0; ordinal < 30U; ++ordinal)
                    requests.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
                        SegmentKind::SGK_ENTITY_PAGE_POOL, 19U, Generation(1U), SegmentState::SGS_SEALED, true});
                const auto prepared = metadata->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U), PhysicalSlotId(69U),
                    LogSequenceNumber(3U), [&](std::span<const CatalogEntry> entries) {
                        for (const auto &entry : entries) prepareEmptyCrashPool(*io, initial.data_region_offset, entry);
                    });
                if (!prepared) return 25;
                const auto original = prepared->entries;
                std::vector<MetadataSnapshotPin> retained;
                for (uint64_t checkpoint = 4U; checkpoint <= 6U; ++checkpoint) {
                    retained.emplace_back(metadata->openSnapshot());
                    static_cast<void>(metadata->upsertCatalog(original, LogSequenceNumber(checkpoint)));
                }
                for (auto &snapshot : retained) snapshot.release();

				StorageIO::ScopedNativeWriteHookForTesting pauseAfterPublication(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
								if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return persisted;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(metadata->reclaimEmptyMetadataPagePools(LogSequenceNumber(7U)));
				return 26;
			}
			if (mode == CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_CANDIDATE ||
				mode == CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_AFTER_PUBLICATION) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 73;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNewComplete(io, initial);
                std::vector<CatalogSlotReservationRequest> requests;
                for (uint64_t ordinal = 0; ordinal < 30U; ++ordinal)
                    requests.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
                        SegmentKind::SGK_ENTITY_PAGE_POOL, 19U, Generation(1U), SegmentState::SGS_SEALED, true});
                const auto prepared = metadata->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U), PhysicalSlotId(69U),
                    LogSequenceNumber(3U), [&](std::span<const CatalogEntry> entries) {
                        for (const auto &entry : entries) prepareEmptyCrashPool(*io, initial.data_region_offset, entry);
                    });
                if (!prepared) return 73;
                const auto original = prepared->entries;
                std::vector<MetadataSnapshotPin> retained;
                for (uint64_t checkpoint = 4U; checkpoint <= 6U; ++checkpoint) {
                    retained.emplace_back(metadata->openSnapshot());
                    static_cast<void>(metadata->upsertCatalog(original, LogSequenceNumber(checkpoint)));
                }
                for (auto &snapshot : retained) snapshot.release();
                {
                    PhysicalSegmentAllocator segments(io, *metadata); VectorCodebookStore codebooks(segments, *metadata);
                    VectorCodebookWriteRequest request; request.codebook_id = 77U; request.manifest_segment_id = SegmentId(1000000U);
                    request.generation = Generation(1U); request.element_type = VectorCodebookElementType::VCET_FLOAT32;
                    request.dimensions = 2U; request.subspace_count = 1U; request.centroid_count = 3U;
                    const std::array<std::byte, 24U> payload{};
                    static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(80U), PhysicalSlotId(80U), LogSequenceNumber(7U)));
                }
                static_cast<void>(metadata->upsertCatalog(std::span(&original.front(), 1U), LogSequenceNumber(8U)));
                auto pinned = metadata->openSnapshot();
                const auto extent = metadata->findExtentLocation(pinned, VectorCodebookExtentIdCodec::manifestExtentId(77U));
                pinned.release();
                static_cast<void>(metadata->upsertLocations({}, std::span(&extent, 1U), LogSequenceNumber(9U)));

				StorageIO::ScopedNativeWriteHookForTesting pauseAtRelocationBoundary(
						[mode, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (mode == CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_AFTER_PUBLICATION && offset >= 0 &&
								static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return written;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							bool catalogLeaf = false;
							if (mode == CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_CANDIDATE && offset >= 0 &&
								static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_PAGE_SIZE) {
								try {
									static_cast<void>(CatalogLeafPageCodec::decode(
											std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount)));
									catalogLeaf = true;
								} catch (const FormatException &) {
									// Route and other typed metadata candidates are not Catalog leaves.
								}
							}
							if (catalogLeaf) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return written;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(metadata->evacuateSparseMetadataPagePool(
						LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT));
				return 74;
			}
			if (mode == CRASH_CHILD_TAIL_TRUNCATE_PRE_TRUNCATE_FENCE ||
				mode == CRASH_CHILD_TAIL_TRUNCATE_POST_TRUNCATE_FENCE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 24;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				PhysicalSegmentAllocator allocator(io, *metadata);
				PhysicalSegmentAllocationRequest request;
				request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
				request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
				request.owner_id = 77U;
				request.generation = Generation(1U);
				if (!allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
							 .has_value()) {
					return 25;
				}
				static_cast<void>(allocator.retire(request.segment_id, request.generation, LogSequenceNumber(3U)));
				static_cast<void>(
						allocator.releaseRetired(request.segment_id, request.generation, LogSequenceNumber(4U)));
				uint32_t syncCalls = 0U;
				StorageIO::ScopedNativeSyncHookForTesting pauseAtTruncateBoundary(
						[&syncCalls, &markerPath, &io, mode](file_handle_t fd) {
							if (syncCalls++ == 0U) {
								if (portable_fsync(fd) != 0) {
									return -1;
								}
								if (mode == CRASH_CHILD_TAIL_TRUNCATE_PRE_TRUNCATE_FENCE) {
									std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
									if (!marker.is_open()) {
										return -1;
									}
									marker << io->fileSize() << '\n';
									if (!marker) {
										return -1;
									}
									marker.flush();
									if (!marker) {
										return -1;
									}
									waitToBeKilled();
								}
								return 0;
							}
							if (mode == CRASH_CHILD_TAIL_TRUNCATE_POST_TRUNCATE_FENCE) {
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_fsync(fd);
						});
				static_cast<void>(metadata->truncateReleasedTail());
				return 26;
			}
            if (mode == CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CANDIDATE_SHORT_WRITE ||
                mode == CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CLEANUP) {
                const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
                if (handle == INVALID_FILE_HANDLE) return 27;
                auto io = std::make_shared<StorageIO>(file, handle, handle);
                Superblock initial; initial.database_id = DatabaseId(0x1020304050607080ULL);
                initial.generation = Generation(1U); initial.checkpoint_lsn = LogSequenceNumber(1U);
                auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
                std::vector<CatalogSlotReservationRequest> requests;
                for (uint64_t ordinal = 0; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal)
                    requests.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
                        SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED, true});
                const auto prepared = metadata->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U), PhysicalSlotId(69U),
                    LogSequenceNumber(2U), [&](std::span<const CatalogEntry> entries) {
                        for (const auto &entry : entries) prepareEmptyCrashPool(*io, initial.data_region_offset, entry);
                    });
                if (!prepared) return 27;
                std::vector<MetadataSnapshotPin> retainedRoots;
                uint64_t nextLsn = 3U;
                for (size_t round = 0; metadata->pagePoolStats().free_pages > RSS_PAGE_POOL_DATA_FRAME_COUNT + 4U && round < 32U; ++round) {
                    retainedRoots.emplace_back(metadata->openSnapshot());
                    static_cast<void>(metadata->upsertCatalog(prepared->entries, LogSequenceNumber(nextLsn++)));
                }
                if (metadata->pagePoolStats().free_pages > RSS_PAGE_POOL_DATA_FRAME_COUNT + 4U)
                    throw std::runtime_error("could not prepare a nearly full ordinary metadata pool");
                auto beforeConsume = metadata->openSnapshot();
                PhysicalSlotId ordinarySlot{};
                for (uint64_t slot = 3U; slot < 40U; ++slot) {
                    if (!metadata->isPhysicalSlotAllocated(beforeConsume, PhysicalSlotId(slot))) { ordinarySlot = PhysicalSlotId(slot); break; }
                }
                beforeConsume.release();
                if (!ordinarySlot.isValid()) return 27;
                const auto ordinaryOffset = PhysicalSegmentAllocator::checkedSlotOffset(initial.data_region_offset, ordinarySlot);
                retainedRoots.emplace_back(metadata->openSnapshot());
                bool denied = false;
                {
                    StorageIO::ScopedNativeWriteHookForTesting consumeReserve([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
                        if (offset >= static_cast<pwrite_off_t>(ordinaryOffset) && offset < static_cast<pwrite_off_t>(ordinaryOffset + RSS_SEGMENT_SIZE)) {
                            denied = true;
#ifdef _WIN32
                            SetLastError(ERROR_DISK_FULL);
#else
                            errno = ENOSPC;
#endif
                            return static_cast<pwrite_ssize_t>(-1);
                        }
                        return portable_pwrite(fd, bytes, count, offset);
                    });
                    static_cast<void>(metadata->upsertCatalog(prepared->entries, LogSequenceNumber(nextLsn++)));
                }
                for (auto &pin : retainedRoots) pin.release();
                if (!denied) throw std::runtime_error("ordinary metadata pool ENOSPC hook was not reached");
                const uint64_t initialFileSize = io->fileSize();
                if (initialFileSize < initial.data_region_offset || (initialFileSize - initial.data_region_offset) % RSS_SEGMENT_SIZE) return 28;
                const auto tail = (initialFileSize - initial.data_region_offset) / RSS_SEGMENT_SIZE;
                auto snapshot = metadata->openSnapshot();
                const bool tailFree = !metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(tail));
                snapshot.release();
                const auto successor = PhysicalSlotId(tailFree ? tail : tail + 1U);
                const auto successorOffset = PhysicalSegmentAllocator::checkedSlotOffset(initial.data_region_offset, successor);
                if (mode == CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CANDIDATE_SHORT_WRITE) {
                    StorageIO::ScopedNativeWriteHookForTesting shortSuccessor([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
                        if (offset >= 0 && static_cast<uint64_t>(offset) == successorOffset && count == RSS_PAGE_SIZE) {
                            const auto partial = portable_pwrite(fd, bytes, 16U, offset);
                            if (partial != 16) return partial;
                            writeMarker(markerPath); waitToBeKilled();
                        }
                        return portable_pwrite(fd, bytes, count, offset);
                    });
                    static_cast<void>(metadata->rotateMetadataEmergencyReserve(LogSequenceNumber(nextLsn)));
                    return 28;
                }
                std::atomic<uint32_t> syncCalls{0U};
                StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&](file_handle_t fd) {
                    if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
#ifdef _WIN32
                        SetLastError(ERROR_DISK_FULL);
#else
                        errno = ENOSPC;
#endif
                        return -1;
                    }
                    return portable_fsync(fd);
                });
                StorageIO::ScopedNativeTruncateHookForTesting killBeforeCleanup([&](file_handle_t fd, uint64_t size) {
                    if (size == initialFileSize) { writeMarker(markerPath); waitToBeKilled(); }
                    return portable_ftruncate(fd, size);
                });
                static_cast<void>(metadata->rotateMetadataEmergencyReserve(LogSequenceNumber(nextLsn)));
                return 32;
            }

			if (mode == CRASH_CHILD_METADATA_POOL_CANDIDATE_SHORT_WRITE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 29;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
				MetadataSnapshotPin bootstrap = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						bootstrap.superblock().data_region_offset, PhysicalSlotId(3U));
				bootstrap.release();
				std::vector<CatalogEntry> entries;
				entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
				for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal) {
					entries.push_back(
							{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
							 PhysicalSlotId(1000U + ordinal), SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U),
							 SegmentState::SGS_SEALED, LogSequenceNumber(2U)});
				}
				StorageIO::ScopedNativeWriteHookForTesting shortPoolCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
													   pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16) {
									return partial;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(metadata->upsertCatalog(entries, LogSequenceNumber(2U)));
				return 30;
			}
			if (mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_BEFORE_PUBLICATION ||
				mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION ||
				mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE || mode == CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE ||
				mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE) {
				const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
				if (handle == INVALID_FILE_HANDLE) {
					return 63;
				}
				auto io = std::make_shared<StorageIO>(file, handle, handle);
				Superblock initial;
				initial.database_id = DatabaseId(0x1020304050607080ULL);
				initial.generation = Generation(1U);
				initial.checkpoint_lsn = LogSequenceNumber(1U);
				const bool routeRadixFixture = mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE;
                const bool routeReclaimFixture = mode == CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM;
                const bool currentRouteFixture = routeRadixFixture ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE ||
                    mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE;
				auto metadata = (currentRouteFixture || routeReclaimFixture) ? RelocatableMetadataStore::initializeNewComplete(io, initial) :
					RelocatableMetadataStore::initializeNew(io, initial);
				const CatalogEntry catalog{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
										   PhysicalSlotId(100U),
										   SegmentKind::SGK_ENTITY_PAGE_POOL,
										   77U,
										   Generation(1U),
										   SegmentState::SGS_SEALED,
										   LogSequenceNumber(2U)};
				if (routeReclaimFixture) {
                    PhysicalSegmentAllocator segments(io, *metadata); VectorCodebookStore codebooks(segments, *metadata);
                    VectorCodebookWriteRequest request; request.codebook_id = 77U; request.manifest_segment_id = catalog.segment_id;
                    request.generation = Generation(1U); request.element_type = VectorCodebookElementType::VCET_FLOAT32;
                    request.dimensions = 2U; request.subspace_count = 1U; request.centroid_count = 3U;
                    const std::array<std::byte, 24U> payload{};
                    static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(100U), PhysicalSlotId(100U), LogSequenceNumber(3U)));
                } else if (mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_BEFORE_PUBLICATION ||
					mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION ||
					mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE) {
					// Publish a real, allocated opaque segment in the same generation
					// as the Catalog entry; recovery must authenticate its envelope.
					std::vector<CatalogSlotReservationRequest> requests{{catalog.segment_id, catalog.kind, catalog.owner_id,
						catalog.generation, catalog.state, true}};
					const bool extentFixture = mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE ||
						mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE;
					if (extentFixture) for (uint64_t id : {20U, 21U})
						requests.push_back({SegmentId(id), SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U),
							SegmentState::SGS_SEALED, true});
					if (routeRadixFixture) {
						const size_t poolCount = (locationsThatRequireRouteRadix().size() + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
							RSS_PAGE_POOL_DATA_FRAME_COUNT;
						for (size_t i = 1; i < poolCount; ++i)
							requests.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + i),
								SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED, true});
					}
					const auto prepared = metadata->reserveFirstFreeAndUpsertCatalog(requests,
						catalog.physical_slot, PhysicalSlotId(99U + requests.size()), LogSequenceNumber(currentRouteFixture ? 3U : 2U),
						[&](std::span<const CatalogEntry> entries) {
							if (entries.size() != requests.size()) throw std::runtime_error("unexpected crash fixture segments");
							if (routeRadixFixture) { prepareDistinctRoutePages(*io, initial.data_region_offset, entries); return; }
							for (const auto &entry : entries) {
							SegmentEnvelope envelope;
							envelope.kind = entry.kind; envelope.state = entry.state; envelope.segment_id = entry.segment_id;
							envelope.owner_id = entry.owner_id; envelope.generation = entry.generation;
							envelope.committed_lsn = entry.committed_lsn;
							std::vector<std::byte> payload(RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES);
							envelope.payload_bytes = static_cast<uint32_t>(payload.size());
							envelope.payload_crc = graph::utils::calculateCrc(payload.data(), payload.size());
							const auto bytes = SegmentEnvelopeCodec::encode(envelope);
							const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(initial.data_region_offset, entry.physical_slot);
							io->writeAt(offset, bytes.data(), bytes.size());
							io->writeAt(offset + RSS_ENVELOPE_BYTES, payload.data(), payload.size());
							}
						});
					if (!prepared) throw std::runtime_error("crash fixture segment reservation failed");
				} else {
					static_cast<void>(metadata->upsertCatalog(std::span(&catalog, 1U), LogSequenceNumber(2U)));
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM) {
					bool published = false;
					StorageIO::ScopedNativeWriteHookForTesting pauseAfterRouteReclaim(
							[&published, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
													  pwrite_off_t offset) {
								if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_SUPERBLOCK_BYTES) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									published = true;
									return written;
								}
								if (published && offset >= 0 &&
									static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
                    PhysicalSegmentAllocator segments(io, *metadata); VectorCodebookStore codebooks(segments, *metadata);
                    const auto relocated = codebooks.relocateExtent(77U, VectorCodebookExtentIdCodec::manifestExtentId(77U),
                        PhysicalSlotId(101U), PhysicalSlotId(101U), LogSequenceNumber(4U));
                    if (!relocated) return 65;
					return 65;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE ||
					mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE) {
					const uint32_t candidateMagic = mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE
														 ? RSS_METADATA_PAGE_ROUTE_LEAF_MAGIC
														 : RSS_PAGE_DIRECTORY_BOOTSTRAP_MAGIC;
					StorageIO::ScopedNativeWriteHookForTesting pauseAtRouteCandidate(
							[candidateMagic, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																	pwrite_off_t offset) {
								const auto *page = static_cast<const std::byte *>(bytes);
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE &&
									LittleEndianCodec::readU32(std::span<const std::byte>(page, 4U), 0U) == candidateMagic) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(101U),
												   SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
												   LogSequenceNumber(4U)};
					static_cast<void>(metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U)));
					return 66;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE) {
					const std::vector<PageLocation> locations = locationsThatRequireRouteRadix();
					static_cast<void>(metadata->upsertLocations(locations, {}, LogSequenceNumber(4U)));
					StorageIO::ScopedNativeWriteHookForTesting pauseAtRouteRadixCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool routeRadix = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										routeRadix = MetadataRadixPageCodec::decode(
													 std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount))
													 .directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
									} catch (const FormatException &) {
										// Candidate leaf/header images have distinct codecs and are not route radix pages.
									}
								}
								if (routeRadix) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					PageLocation replacement = locations.front();
					static_cast<void>(metadata->upsertLocations(std::span(&replacement, 1U), {}, LogSequenceNumber(5U)));
					return 67;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE) {
					StorageIO::ScopedNativeWriteHookForTesting pauseAtCatalogCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool catalogLeaf = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										static_cast<void>(CatalogLeafPageCodec::decode(
												std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount)));
										catalogLeaf = true;
									} catch (const FormatException &) {
										// Other metadata candidates have distinct typed codecs.
									}
								}
								if (catalogLeaf) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(101U),
												   SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
												   LogSequenceNumber(4U)};
					static_cast<void>(metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U)));
					return 68;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE) {
					StorageIO::ScopedNativeWriteHookForTesting pauseAtAllocatorLeafCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool allocatorLeaf = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										static_cast<void>(AllocatorLeafPageCodec::decode(
												std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount)));
										allocatorLeaf = true;
									} catch (const FormatException &) {
										// Allocator leaf candidates have a distinct typed codec.
									}
								}
								if (allocatorLeaf) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					static_cast<void>(metadata->setPhysicalSlotAllocated(PhysicalSlotId(3U), true, LogSequenceNumber(4U)));
					return 69;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE) {
					const PhysicalSlotId firstSlot(
							RSS_ALLOCATOR_PAGE_SLOT_CAPACITY * RSS_CATALOG_RADIX_FANOUT + 1U);
					static_cast<void>(metadata->setPhysicalSlotAllocated(firstSlot, true, LogSequenceNumber(4U)));
					StorageIO::ScopedNativeWriteHookForTesting pauseAtLogicalAllocatorInternalCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool logicalAllocatorRadix = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										const MetadataRadixPage page = MetadataRadixPageCodec::decode(
												std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
										logicalAllocatorRadix = page.directory_type == MetadataDirectoryType::MDT_ALLOCATOR &&
												page.logical_children_enabled;
									} catch (const FormatException &) {
										// The hook is limited to final v5 Allocator radix candidates.
									}
								}
								if (logicalAllocatorRadix) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					static_cast<void>(metadata->setPhysicalSlotAllocated(
							PhysicalSlotId(firstSlot.value() + 1U), true, LogSequenceNumber(5U)));
					return 70;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE) {
					const ExtentId extentId(RSS_LOCATION_LEAF_MAX_ENTRIES * RSS_CATALOG_RADIX_FANOUT + 1U);
					const ExtentLocation original{extentId, SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT,
														  Generation(1U)};
					static_cast<void>(metadata->upsertLocations({}, std::span(&original, 1U), LogSequenceNumber(4U)));
					StorageIO::ScopedNativeWriteHookForTesting pauseAtLogicalLocationInternalCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool logicalExtentRadix = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										const MetadataRadixPage page = MetadataRadixPageCodec::decode(
												std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
										logicalExtentRadix = page.directory_type == MetadataDirectoryType::MDT_EXTENT &&
												page.logical_children_enabled;
									} catch (const FormatException &) {
										// The hook is limited to final v5 Extent-directory radix candidates.
									}
								}
								if (logicalExtentRadix) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					ExtentLocation replacement = original;
					replacement.segment_id = SegmentId(21U);
					static_cast<void>(metadata->upsertLocations({}, std::span(&replacement, 1U), LogSequenceNumber(5U)));
					return 71;
				}
				if (mode == CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE) {
					const ExtentLocation original{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT,
												  Generation(1U)};
					static_cast<void>(metadata->upsertLocations({}, std::span(&original, 1U), LogSequenceNumber(4U)));
					StorageIO::ScopedNativeWriteHookForTesting pauseAtLocationLeafCandidate(
							[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
								bool locationLeaf = false;
								if (offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
									byteCount == RSS_PAGE_SIZE) {
									try {
										locationLeaf =
												LocationLeafPageCodec::decode(
														std::span<const std::byte>(
																static_cast<const std::byte *>(bytes), byteCount))
														.directory_type == MetadataDirectoryType::MDT_EXTENT;
									} catch (const FormatException &) {
										// Other candidate pages have distinct codecs.
									}
								}
								if (locationLeaf) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
									writeMarker(markerPath);
									waitToBeKilled();
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					ExtentLocation replacement = original;
					replacement.segment_id = SegmentId(21U);
					static_cast<void>(
							metadata->upsertLocations({}, std::span(&replacement, 1U), LogSequenceNumber(5U)));
					return 72;
				}
				StorageIO::ScopedNativeWriteHookForTesting pauseAtBootstrapPublication(
						[mode, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
											pwrite_off_t offset) {
							if (mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE && offset >= 0 &&
								static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return written;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								if (mode == CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION) {
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
										return written;
									}
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(metadata->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
				return 64;
			}
			auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			SuperblockFile bootstrap(io);
			bootstrap.initializeNew(superblock(1, 10, 100));

			const std::array<std::byte, 128> durableData{};
			io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, durableData.data(), durableData.size());
			io->sync();
			if (mode == CRASH_CHILD_AFTER_PUBLICATION) {
				bootstrap.publish(superblock(2, 11, 200));
			}
			writeMarker(markerPath);
			waitToBeKilled();
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 4;
		} catch (...) {
			writeFailureMarker(markerPath, "unknown exception");
			return 5;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath =
				std::filesystem::temp_directory_path() / ("relocatable_process_crash_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	[[nodiscard]] Superblock recover() const {
		auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		EXPECT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		return SuperblockFile(std::move(io)).recover();
	}
};

TEST_F(RelocatableProcessCrashTest, KillBeforePublicationRetainsTheLastDurableGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	EXPECT_EQ(recover().generation, Generation(1));
	EXPECT_EQ(recover().catalog_root.physical_slot, PhysicalSlotId(100));
}

TEST_F(RelocatableProcessCrashTest, KillAfterPublicationRecoversTheNewDurableGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	EXPECT_EQ(recover().generation, Generation(2));
	EXPECT_EQ(recover().catalog_root.physical_slot, PhysicalSlotId(200));
}

TEST_F(RelocatableProcessCrashTest, KillAfterSegmentAllocationPublicationReopensTheCompleteSegmentAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40U));
	EXPECT_EQ(allocator.read(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).payload,
			  (std::vector<std::byte>{std::byte{0xB3}, std::byte{0xC4}}));
	recovered.release();

	PhysicalSegmentAllocationRequest replacement;
	replacement.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	replacement.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	replacement.owner_id = 78U;
	replacement.generation = Generation(1U);
	replacement.payload = {std::byte{0xD5}};
	ASSERT_TRUE(allocator.allocate(replacement, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U))
					.has_value());
}

TEST_F(RelocatableProcessCrashTest, KillAfterSegmentAllocationDataFenceRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SEGMENT_ALLOCATION_AFTER_DATA_FENCE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID))),
				 FormatException);
	recovered.release();

	PhysicalSegmentAllocationRequest retry;
	retry.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	retry.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	retry.owner_id = 77U;
	retry.generation = Generation(1U);
	retry.payload = {std::byte{0xA3}, std::byte{0xB4}};
	ASSERT_TRUE(allocator.allocate(retry, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)).has_value());
}

TEST_F(RelocatableProcessCrashTest, KillAfterAllocatorDirectoryDataFenceRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_DATA_FENCE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	SuperblockFile bootstrap(io);
	const Superblock recovered = bootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(1U));
	MetadataPageFile pages(io, recovered);
	MetadataPagePoolAllocator allocator(io, recovered);
	AllocatorDirectory directory(pages, allocator);
	const Superblock published = directory.setAllocatedAndPublish(
			bootstrap, recovered, PhysicalSlotId(1U), true, LogSequenceNumber(2U));
	EXPECT_EQ(published.generation, Generation(2U));
	EXPECT_TRUE(directory.isAllocated(published, PhysicalSlotId(1U)));
}

TEST_F(RelocatableProcessCrashTest, KillAfterAllocatorDirectoryPublicationReopensTheCompleteGenerationAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ALLOCATOR_DIRECTORY_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	SuperblockFile bootstrap(io);
	const Superblock recovered = bootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2U));
	MetadataPageFile pages(io, recovered);
	MetadataPagePoolAllocator allocator(io, recovered);
	AllocatorDirectory directory(pages, allocator);
	EXPECT_TRUE(directory.isAllocated(recovered, PhysicalSlotId(1U)));
	const Superblock next = directory.setAllocatedAndPublish(
			bootstrap, recovered, PhysicalSlotId(2U), true, LogSequenceNumber(3U));
	EXPECT_EQ(next.generation, Generation(3U));
	EXPECT_TRUE(directory.isAllocated(next, PhysicalSlotId(2U)));
}

TEST_F(RelocatableProcessCrashTest, KillBeforeBootstrapMigrationPublicationRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_MIGRATION_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_FALSE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const Superblock retried = metadata->migratePageDirectoryToBootstrap(LogSequenceNumber(3U));
	EXPECT_EQ(retried.generation, Generation(3U));
	EXPECT_TRUE(retried.usesPageDirectoryBootstrap());
}

TEST_F(RelocatableProcessCrashTest, KillDuringBootstrapMigrationCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_MIGRATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_FALSE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const Superblock retried = metadata->migratePageDirectoryToBootstrap(LogSequenceNumber(3U));
	EXPECT_EQ(retried.generation, Generation(3U));
	EXPECT_TRUE(retried.usesPageDirectoryBootstrap());
}

TEST_F(RelocatableProcessCrashTest, KillAfterBootstrapRouteReclaimRecoversPublishedGenerationAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_ROUTE_RECLAIM);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(101U));
	recovered.release();

    {
    PhysicalSegmentAllocator segments(io, *metadata);
    static_cast<void>(segments.releaseRelocatedSource(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), Generation(2U), LogSequenceNumber(5U)));
    VectorCodebookStore codebooks(segments, *metadata);
    const auto relocated = codebooks.relocateExtent(77U, VectorCodebookExtentIdCodec::manifestExtentId(77U),
        PhysicalSlotId(102U), PhysicalSlotId(102U), LogSequenceNumber(6U));
    ASSERT_TRUE(relocated);
    EXPECT_EQ(relocated->superblock.generation, Generation(6U));
    }
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
    EXPECT_EQ(reopened->findCatalog(persisted, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(102U));
    persisted.release();
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableProcessCrashTest,
	   KillAfterMetadataPoolReclaimPublicationReopensWithReleasedPoolSlotReusable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_METADATA_POOL_RECLAIM_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	ASSERT_EQ(recovered.superblock().generation, Generation(7U));
	ASSERT_GE(metadata->pagePoolStats().pools, 1U);

	std::vector<PhysicalSlotId> currentMetadataPools;
	metadata->visitCatalogEntries(recovered, [&currentMetadataPools](const CatalogEntry &entry) {
		if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL && entry.state != SegmentState::SGS_OBSOLETE) {
			currentMetadataPools.push_back(entry.physical_slot);
		}
	});
	ASSERT_FALSE(currentMetadataPools.empty());

	// The released pool header remains on disk even though its durable Catalog
	// entry and allocator bit are gone. Find that exact slot instead of merely
	// choosing an unrelated free tail slot.
	std::optional<PhysicalSlotId> releasedSlot;
	const uint64_t slotCount =
			(io->fileSize() - recovered.superblock().data_region_offset) / RSS_SEGMENT_SIZE;
	for (uint64_t rawSlot = RSS_FIRST_ALLOCATABLE_SEGMENT_ID; rawSlot <= slotCount; ++rawSlot) {
		const PhysicalSlotId slot(rawSlot);
		if (std::ranges::find(currentMetadataPools, slot) != currentMetadataPools.end() ||
			metadata->isPhysicalSlotAllocated(recovered, slot)) {
			continue;
		}
		const uint64_t headerOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(recovered.superblock().data_region_offset, slot) +
				RSS_ENVELOPE_BYTES;
		std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> firstHeaderBytes{};
		std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> secondHeaderBytes{};
		if (io->readAt(headerOffset, firstHeaderBytes.data(), firstHeaderBytes.size()) != firstHeaderBytes.size() ||
			io->readAt(headerOffset + RSS_METADATA_PAGE_POOL_HEADER_BYTES, secondHeaderBytes.data(),
					secondHeaderBytes.size()) != secondHeaderBytes.size()) {
			continue;
		}
		try {
			const MetadataPagePoolHeader first = MetadataPagePoolHeaderCodec::decode(firstHeaderBytes);
			const MetadataPagePoolHeader second = MetadataPagePoolHeaderCodec::decode(secondHeaderBytes);
			const MetadataPagePoolHeader &header =
					first.control_sequence >= second.control_sequence ? first : second;
			if (header.segment_id.isValid() && header.generation.isValid() && header.committed_lsn.isValid() &&
				header.allocated_frames == 0U) {
				releasedSlot = slot;
				break;
			}
		} catch (const FormatException &) {
			// Most physical slots are ordinary data or have no metadata-pool
			// header; only a valid empty header can identify the released pool.
		}
	}
	ASSERT_TRUE(releasedSlot.has_value());
	const PhysicalSlotId slot = *releasedSlot;
	bool staleCatalogOwnership = false;
	metadata->visitCatalogEntries(recovered, [slot, &staleCatalogOwnership](const CatalogEntry &entry) {
		if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL && entry.physical_slot == slot) {
			staleCatalogOwnership = true;
		}
	});
	EXPECT_FALSE(staleCatalogOwnership);
	recovered.release();

	CatalogSlotReservationRequest request;
	request.segment_id = SegmentId(10000U);
	request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	request.owner_id = 19U;
	request.segment_generation = Generation(1U);
	request.state = SegmentState::SGS_SEALED;
    const std::array requests{request};
    const auto reused = metadata->reserveFirstFreeAndUpsertCatalog(requests, slot, slot, LogSequenceNumber(8U),
        [&](std::span<const CatalogEntry> entries) { prepareEmptyCrashPool(*io, RSS_FILE_BOOTSTRAP_BYTES, entries.front()); });
    ASSERT_TRUE(reused.has_value()); ASSERT_EQ(reused->entries.size(), 1U);
    EXPECT_EQ(reused->entries.front().physical_slot, slot);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(persisted, request.segment_id).physical_slot, slot);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(persisted, slot));
    persisted.release();
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableProcessCrashTest,
	   KillDuringRoutedMetadataPoolRelocationCandidateRetainsOldGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(9U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(metadata->findExtentLocation(recovered, VectorCodebookExtentIdCodec::manifestExtentId(77U)),
              (ExtentLocation{VectorCodebookExtentIdCodec::manifestExtentId(77U), SegmentId(1000000U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)}));
	recovered.release();

	const MetadataPagePoolEvacuation retried =
			metadata->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(persisted.superblock().generation, Generation(10U));
	EXPECT_EQ(reopened->findCatalog(persisted, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(reopened->findExtentLocation(persisted, VectorCodebookExtentIdCodec::manifestExtentId(77U)),
              (ExtentLocation{VectorCodebookExtentIdCodec::manifestExtentId(77U), SegmentId(1000000U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)}));
    persisted.release(); EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableProcessCrashTest,
	   KillAfterRoutedMetadataPoolRelocationPublicationReopensTheNewGenerationAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ROUTED_METADATA_POOL_RELOCATION_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(10U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(metadata->findExtentLocation(recovered, VectorCodebookExtentIdCodec::manifestExtentId(77U)),
              (ExtentLocation{VectorCodebookExtentIdCodec::manifestExtentId(77U), SegmentId(1000000U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)}));
	recovered.release();

    {
        PhysicalSegmentAllocator segments(io, *metadata); VectorCodebookStore codebooks(segments, *metadata);
        const auto moved = codebooks.relocateExtent(77U, VectorCodebookExtentIdCodec::manifestExtentId(77U),
            PhysicalSlotId(81U), PhysicalSlotId(81U), LogSequenceNumber(11U));
        ASSERT_TRUE(moved); EXPECT_EQ(moved->superblock.generation, Generation(11U));
    }
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(persisted.superblock().generation, Generation(11U));
	EXPECT_EQ(reopened->findCatalog(persisted, SegmentId(1000000U)).physical_slot, PhysicalSlotId(81U));
    persisted.release(); EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationRouteCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(101U),
								   SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
								   LogSequenceNumber(4U)};
	const Superblock retried = metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.generation, Generation(4U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(persisted, replacement.segment_id).physical_slot, replacement.physical_slot);
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationHeaderCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_HEADER_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(101U),
								   SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
								   LogSequenceNumber(4U)};
	const Superblock retried = metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.generation, Generation(4U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(persisted, replacement.segment_id).physical_slot, replacement.physical_slot);
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationRouteRadixCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ROUTE_RADIX_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findPageLocation(recovered, PageId(1U)).free_bytes, 0U);
	const auto oldRoute = recovered.superblock().page_directory_root;
	const auto oldGraphRevision = recovered.superblock().graph_revision;
	const auto oldSchemaRevision = recovered.superblock().schema_revision;
	const auto lastLocation = locationsThatRequireRouteRadix().back();
	EXPECT_EQ(metadata->findPageLocation(recovered, lastLocation.page_id), lastLocation);
	EXPECT_NE(lastLocation.segment_id, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
	recovered.release();

	PageLocation replacement{PageId(1U), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), RSS_PAGE_POOL_FIRST_DATA_FRAME,
							 Generation(1U), 0U};
	const Superblock retried = metadata->upsertLocations(std::span(&replacement, 1U), {}, LogSequenceNumber(5U));
	EXPECT_EQ(retried.generation, Generation(5U));
	EXPECT_NE(retried.page_directory_root, oldRoute);
	EXPECT_EQ(retried.graph_revision, oldGraphRevision);
	EXPECT_EQ(retried.schema_revision, oldSchemaRevision);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(persisted, replacement.page_id).free_bytes, replacement.free_bytes);
	EXPECT_EQ(reopened->findPageLocation(persisted, lastLocation.page_id), lastLocation);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationCatalogCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_CATALOG_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(101U),
							   SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
							   LogSequenceNumber(4U)};
	const Superblock retried = metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.generation, Generation(4U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(persisted, replacement.segment_id).physical_slot, replacement.physical_slot);
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationAllocatorCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
	recovered.release();

	const Superblock retried = metadata->setPhysicalSlotAllocated(PhysicalSlotId(3U), true, LogSequenceNumber(4U));
	EXPECT_EQ(retried.generation, Generation(4U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(persisted, PhysicalSlotId(3U)));
}

TEST_F(RelocatableProcessCrashTest,
		KillDuringPostMigrationAllocatorInternalCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_ALLOCATOR_INTERNAL_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const PhysicalSlotId firstSlot(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY * RSS_CATALOG_RADIX_FANOUT + 1U);
	const PhysicalSlotId replacementSlot(firstSlot.value() + 1U);
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, firstSlot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, replacementSlot));
	recovered.release();

	const Superblock retried = metadata->setPhysicalSlotAllocated(replacementSlot, true, LogSequenceNumber(5U));
	EXPECT_EQ(retried.generation, Generation(5U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(persisted, firstSlot));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(persisted, replacementSlot));
}

TEST_F(RelocatableProcessCrashTest,
		KillDuringPostMigrationLocationInternalCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_INTERNAL_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const ExtentId extentId(RSS_LOCATION_LEAF_MAX_ENTRIES * RSS_CATALOG_RADIX_FANOUT + 1U);
	const ExtentLocation original{extentId, SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)};
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findExtentLocation(recovered, extentId), original);
	recovered.release();

	ExtentLocation replacement = original;
	replacement.segment_id = SegmentId(21U);
	const Superblock retried = metadata->upsertLocations({}, std::span(&replacement, 1U), LogSequenceNumber(5U));
	EXPECT_EQ(retried.generation, Generation(5U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findExtentLocation(persisted, extentId), replacement);
}

TEST_F(RelocatableProcessCrashTest, KillDuringPostMigrationLocationCandidateRetainsPublishedGenerationAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_POST_MIGRATION_LOCATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	const ExtentLocation original{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)};
	EXPECT_EQ(metadata->findExtentLocation(recovered, original.extent_id), original);
	recovered.release();

	ExtentLocation replacement = original;
	replacement.segment_id = SegmentId(21U);
	const Superblock retried = metadata->upsertLocations({}, std::span(&replacement, 1U), LogSequenceNumber(5U));
	EXPECT_EQ(retried.generation, Generation(5U));
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_EQ(reopened->findExtentLocation(persisted, replacement.extent_id), replacement);
}

TEST_F(RelocatableProcessCrashTest, KillAfterBootstrapMigrationPublicationRecoversNewGenerationAndRemainsWritable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BOOTSTRAP_MIGRATION_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100U));
	recovered.release();

	const CatalogEntry replacement{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
								   PhysicalSlotId(101U),
								   SegmentKind::SGK_ENTITY_PAGE_POOL,
								   77U,
								   Generation(1U),
								   SegmentState::SGS_SEALED,
								   LogSequenceNumber(4U)};
	const Superblock updated = metadata->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U));
	EXPECT_EQ(updated.generation, Generation(4U));
	EXPECT_TRUE(updated.usesPageDirectoryBootstrap());
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin persisted = reopened->openSnapshot();
	EXPECT_TRUE(persisted.superblock().usesPageDirectoryBootstrap());
	EXPECT_EQ(reopened->findCatalog(persisted, replacement.segment_id).physical_slot, replacement.physical_slot);
}

TEST_F(RelocatableProcessCrashTest, KillAfterMetadataPoolControlShortWriteRetainsThePreviousBitmap) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_METADATA_POOL_CONTROL_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	const Superblock recovered = SuperblockFile(io).recover();
	MetadataPagePoolAllocator allocator(io, recovered, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
	EXPECT_EQ(allocator.stats().allocated_pages, 1U);
	const PhysicalPageRef retried = allocator.allocatePage(Generation(1U));
	EXPECT_EQ(retried.physical_slot, PhysicalSlotId(1U));
	EXPECT_EQ(retried.frame_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
}

TEST_F(RelocatableProcessCrashTest, KillDuringSegmentCandidateShortWriteRetainsThePublishedGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SEGMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID))),
				 FormatException);
	recovered.release();

	PhysicalSegmentAllocator allocator(io, *metadata);
	PhysicalSegmentAllocationRequest request;
	request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	request.owner_id = 77U;
	request.generation = Generation(1U);
	request.payload = {std::byte{0xC1}};
	EXPECT_TRUE(
			allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)).has_value());
}

TEST_F(RelocatableProcessCrashTest, KillDuringSegmentRelocationCandidateShortWriteRetainsThePublishedSource) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SEGMENT_RELOCATION_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(41U)));
	EXPECT_EQ(allocator.read(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).payload,
			  (std::vector<std::byte>{std::byte{0xF1}, std::byte{0xF2}}));
	recovered.release();
	EXPECT_TRUE(allocator
						.relocate(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), Generation(1U), PhysicalSlotId(41U),
								  PhysicalSlotId(41U), LogSequenceNumber(3U))
						.has_value());
}

TEST_F(RelocatableProcessCrashTest, KillDuringRetiredSegmentReleaseCandidateShortWriteRetainsTheRetireQueue) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_RETIRED_SEGMENT_RELEASE_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_RETIRED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_TRUE(metadata->findRetireQueueEntryOptional(recovered, PhysicalSlotId(40U)).has_value());
	recovered.release();
	EXPECT_EQ(
			allocator.releaseRetired(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), Generation(1U), LogSequenceNumber(4U))
					.generation,
			Generation(4U));
}

TEST_F(RelocatableProcessCrashTest, KillAfterRetiredSegmentReleasePublicationKeepsTheReleasedSlotReusable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_RETIRED_SEGMENT_RELEASE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->findRetireQueueEntryOptional(recovered, PhysicalSlotId(40U)).has_value());
	recovered.release();

	PhysicalSegmentAllocationRequest replacement;
	replacement.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	replacement.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	replacement.owner_id = 78U;
	replacement.generation = Generation(1U);
	replacement.payload = {std::byte{0xC2}};
	ASSERT_TRUE(allocator.allocate(replacement, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(5U))
					.has_value());
	MetadataSnapshotPin reused = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(reused, replacement.segment_id).physical_slot, PhysicalSlotId(40U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(reused, PhysicalSlotId(40U)));
}

TEST_F(RelocatableProcessCrashTest, KillDuringRetireQueueReclaimCandidateShortWriteRetainsTheRetiredSlot) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_RETIRE_QUEUE_RECLAIM_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_RETIRED);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_TRUE(metadata->findRetireQueueEntryOptional(recovered, PhysicalSlotId(40U)).has_value());
	recovered.release();
	EXPECT_EQ(metadata->reclaimRetiredPhysicalSlots(LogSequenceNumber(4U)).reclaimed_physical_slots, 1U);
}

TEST_F(RelocatableProcessCrashTest, KillAfterRetireQueueReclaimPublicationKeepsTheReleasedSlotReusable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_RETIRE_QUEUE_RECLAIM_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->findRetireQueueEntryOptional(recovered, PhysicalSlotId(40U)).has_value());
	recovered.release();

	PhysicalSegmentAllocationRequest replacement;
	replacement.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	replacement.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	replacement.owner_id = 78U;
	replacement.generation = Generation(1U);
	replacement.payload = {std::byte{0xC3}};
	ASSERT_TRUE(allocator.allocate(replacement, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(5U))
					.has_value());
	MetadataSnapshotPin reused = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(reused, replacement.segment_id).physical_slot, PhysicalSlotId(40U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(reused, PhysicalSlotId(40U)));
}

TEST_F(RelocatableProcessCrashTest, KillAfterRelocatedSourceReleasePublicationKeepsTheSourceSlotReusable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_RELOCATED_SOURCE_RELEASE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(41U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(41U)));
	EXPECT_FALSE(metadata->findRetireQueueEntryOptional(recovered, PhysicalSlotId(40U)).has_value());
	EXPECT_EQ(allocator.read(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).payload,
			  (std::vector<std::byte>{std::byte{0xB1}, std::byte{0xB2}}));
	recovered.release();

	PhysicalSegmentAllocationRequest replacement;
	replacement.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	replacement.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	replacement.owner_id = 78U;
	replacement.generation = Generation(1U);
	replacement.payload = {std::byte{0xC5}};
	ASSERT_TRUE(allocator.allocate(replacement, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(6U))
					.has_value());
}

TEST_F(RelocatableProcessCrashTest, KillAfterTailTruncatePreFenceRetainsTailAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_TAIL_TRUNCATE_PRE_TRUNCATE_FENCE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	std::ifstream marker(markerPath, std::ios::binary);
	ASSERT_TRUE(marker.is_open());
	uint64_t originalBytes = 0U;
	ASSERT_TRUE(marker >> originalBytes);

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	const file_handle_t nativeFd = portable_open_rw(databasePath.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
	auto metadata = RelocatableMetadataStore::open(io);
	EXPECT_EQ(io->fileSize(), originalBytes);

	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();

	const RelocatableTailTruncationResult retried = metadata->truncateReleasedTail();
	EXPECT_EQ(retried.status, RelocatableTailTruncationStatus::VTTS_TRUNCATED);
	EXPECT_EQ(retried.old_file_bytes, originalBytes);
	EXPECT_LT(retried.new_file_bytes, originalBytes);
	EXPECT_EQ(io->fileSize(), retried.new_file_bytes);

	metadata.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeFd), 0);
}

TEST_F(RelocatableProcessCrashTest, KillAfterTailTruncateBeforeFinalFenceReopensSafely) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_TAIL_TRUNCATE_POST_TRUNCATE_FENCE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).state,
			  SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
	EXPECT_EQ(metadata->truncateReleasedTail().status, RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE);
}

TEST_F(RelocatableProcessCrashTest, KillDuringEmergencyReserveRotationCandidateRetainsTheConsumedReserve) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const Superblock published = recovered.superblock();
    EXPECT_GT(published.generation.value(), 2U);
	EXPECT_EQ(recovered.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(2U)));
	const uint64_t size = io->fileSize();
    const uint64_t relative = size - recovered.superblock().data_region_offset;
    const uint64_t tail = (relative + RSS_SEGMENT_SIZE - 1U) / RSS_SEGMENT_SIZE;
    const PhysicalSlotId candidate(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(tail)) ? tail + 1U : tail);
    EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, candidate));
    EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(40U));
	recovered.release();
	const MetadataEmergencyReserveRotation rotation = metadata->rotateMetadataEmergencyReserve(LogSequenceNumber(published.checkpoint_lsn.value() + 1U));
	ASSERT_TRUE(rotation.rotated);
	EXPECT_EQ(rotation.current_reserve.physical_slot, candidate);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableProcessCrashTest,
	   KillDuringEmergencyReserveRotationCleanupRetainsPublishedRootsAndReusesTheTailOnRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_EMERGENCY_RESERVE_ROTATION_CLEANUP);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const Superblock published = recovered.superblock();
	EXPECT_GT(published.generation.value(), 2U);
	EXPECT_EQ(published.metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(2U)));
	const uint64_t size = io->fileSize();
    const uint64_t relative = size - recovered.superblock().data_region_offset;
    const uint64_t tail = (relative + RSS_SEGMENT_SIZE - 1U) / RSS_SEGMENT_SIZE;
    const PhysicalSlotId candidate(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(tail)) ? tail + 1U : tail);
    EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, candidate));
    EXPECT_EQ(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot, PhysicalSlotId(40U));
	const uint64_t candidateOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(published.data_region_offset, candidate);
	recovered.release();

	const MetadataEmergencyReserveRotation rotation = metadata->rotateMetadataEmergencyReserve(LogSequenceNumber(published.checkpoint_lsn.value() + 1U));
	ASSERT_TRUE(rotation.rotated);
	EXPECT_EQ(rotation.current_reserve.physical_slot, candidate);
	EXPECT_EQ(io->fileSize(), candidateOffset + RSS_SEGMENT_SIZE);
	MetadataSnapshotPin retried = metadata->openSnapshot();
	EXPECT_EQ(retried.superblock().generation.value(), published.generation.value() + 1U);
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(retried, candidate));
	retried.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableProcessCrashTest, KillDuringMetadataPageCandidateShortWriteRetainsThePublishedGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_METADATA_PAGE_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(metadata->findCatalog(recovered, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID))),
				 FormatException);
	recovered.release();

	PhysicalSegmentAllocator allocator(io, *metadata);
	PhysicalSegmentAllocationRequest request;
	request.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	request.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	request.owner_id = 77U;
	request.generation = Generation(1U);
	request.payload = {std::byte{0xE1}};
	EXPECT_TRUE(
			allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)).has_value());
}

TEST_F(RelocatableProcessCrashTest, KillDuringMetadataPoolCandidateReusesTheRootFreePhysicalTailOnRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_METADATA_POOL_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const Superblock published = recovered.superblock();
	EXPECT_EQ(published.generation, Generation(1U));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
	const uint64_t candidateOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(published.data_region_offset, PhysicalSlotId(3U));
	recovered.release();

	std::vector<CatalogEntry> entries;
	entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
	for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal) {
		entries.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
						   PhysicalSlotId(1000U + ordinal), SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U),
						   SegmentState::SGS_SEALED, LogSequenceNumber(2U)});
	}
	EXPECT_NO_THROW(static_cast<void>(metadata->upsertCatalog(entries, LogSequenceNumber(2U))));
	EXPECT_EQ(io->fileSize(), candidateOffset + RSS_SEGMENT_SIZE);
	MetadataSnapshotPin retried = metadata->openSnapshot();
	EXPECT_EQ(retried.superblock().generation, Generation(2U));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(retried, PhysicalSlotId(3U)));
	EXPECT_EQ(metadata->findCatalog(retried, entries.front().segment_id).physical_slot, entries.front().physical_slot);
}
