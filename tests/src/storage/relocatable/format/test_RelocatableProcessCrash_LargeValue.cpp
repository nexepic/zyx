/**
 * @file test_RelocatableProcessCrash_LargeValue.cpp
 * @brief Real child-process crash coverage for native LargeValue publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <atomic>
#include <cerrno>
#include <cstddef>
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
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_SHORT_WRITE =
			"large-value-data-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_SHORT_WRITE =
			"large-value-manifest-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_METADATA_CANDIDATE_SHORT_WRITE =
			"large-value-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_DATA_FENCE_FAILURE = "large-value-data-fence-failure";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_DISK_FULL =
			"large-value-data-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_DISK_FULL =
			"large-value-manifest-candidate-disk-full";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC =
			"large-value-after-superblock-write-before-final-sync";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MANIFEST_EVACUATION_CANDIDATE =
			"large-value-manifest-evacuation-candidate";

	LargeValueWriteOptions writeOptions() {
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(40U);
		options.last_data_slot = PhysicalSlotId(80U);
		options.first_manifest_slot = PhysicalSlotId(81U);
		options.last_manifest_slot = PhysicalSlotId(90U);
		options.kind = LargeValueKind::LVK_BYTES;
		options.media_type = "application/octet-stream";
		return options;
	}

	const std::vector<std::byte> FIRST_VALUE(17U, std::byte{0x31});
	const std::vector<std::byte> SECOND_VALUE(19U, std::byte{0x32});

	using graph::test::relocatable_crash::CrashChild;

} // namespace

namespace graph::test {

	int runRelocatableLargeValueCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
										  const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_LARGE_VALUE_METADATA_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_LARGE_VALUE_DATA_FENCE_FAILURE &&
			mode != CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_DISK_FULL &&
			mode != CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_DISK_FULL &&
			mode != CRASH_CHILD_LARGE_VALUE_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC &&
			mode != CRASH_CHILD_LARGE_VALUE_MANIFEST_EVACUATION_CANDIDATE) {
			return -1;
		}
		try {
			auto file = std::make_shared<std::fstream>(databasePath,
					std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			if (!file->is_open()) return 110;
			const file_handle_t handle = portable_open_rw(databasePath.string().c_str());
			if (handle == INVALID_FILE_HANDLE) return 111;
			auto io = std::make_shared<StorageIO>(file, handle, handle);
			Superblock initial;
			initial.database_id = DatabaseId(0x1020304050607080ULL);
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
			LargeValueStore values(io, *metadata);
			const LargeValueWriteResult written = values.write(FIRST_VALUE, writeOptions(), LogSequenceNumber(2U));
			if (mode == CRASH_CHILD_LARGE_VALUE_MANIFEST_EVACUATION_CANDIDATE) {
				MetadataSnapshotPin sourceSnapshot = metadata->openSnapshot();
				const LargeValueManifest original = values.manifest(sourceSnapshot, written.reference);
				sourceSnapshot.release();

				LargeValueManifest spare = original;
				spare.page_id = PageId(502U);
				const PageOwnerHeader spareOwner{
						spare.page_id, original.value_id.value(), PageOwnerKind::POK_LARGE_VALUE_MANIFEST, 0U,
						RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(2U), LogSequenceNumber(3U)};
				SharedPagePoolAllocationRequest destinationRequest;
				destinationRequest.segment_id = SegmentId(72U);
				destinationRequest.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				destinationRequest.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				destinationRequest.generation = Generation(2U);
				destinationRequest.page_id = spare.page_id;
				destinationRequest.page_owner_id = original.value_id.value();
				destinationRequest.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
				destinationRequest.payload = LargeValueManifestPage::create(spareOwner, spare).serialize();
				SharedPagePoolAllocator pools(io, *metadata);
				const auto destination = pools.allocateNewPoolPage(
						destinationRequest, PhysicalSlotId(101U), PhysicalSlotId(101U), LogSequenceNumber(3U));
				if (!destination.has_value()) return 116;
				MetadataSnapshotPin destinationSnapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						destinationSnapshot.superblock().data_region_offset, destination->pool.physical_slot) +
						static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
				destinationSnapshot.release();

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
				static_cast<void>(values.evacuate(written.reference.value_id, SegmentId(72U), LogSequenceNumber(4U)));
				return 117;
			}
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const LargeValueManifest firstManifest = values.manifest(snapshot, written.reference);
			const bool interruptManifest = mode == CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_SHORT_WRITE ||
									   mode == CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_DISK_FULL;
			const bool interruptMetadata = mode == CRASH_CHILD_LARGE_VALUE_METADATA_CANDIDATE_SHORT_WRITE;
			const PageLocation firstLocation = metadata->findPageLocation(
					snapshot, interruptManifest ? firstManifest.page_id : firstManifest.chunks.front().first_data_page_id);
			const CatalogEntry pool = metadata->findCatalog(snapshot, firstLocation.segment_id);
			const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, pool.physical_slot) +
					static_cast<uint64_t>(firstLocation.page_index + 1U) * RSS_PAGE_SIZE;
			const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
			snapshot.release();
			if (mode == CRASH_CHILD_LARGE_VALUE_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC) {
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterSuperblockWrite(
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
				static_cast<void>(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)));
				return 112;
			}
			if (mode == CRASH_CHILD_LARGE_VALUE_DATA_FENCE_FAILURE) {
				std::atomic<uint32_t> syncCalls{0U};
				StorageIO::ScopedNativeSyncHookForTesting failFirstFence([&syncCalls](file_handle_t fd) {
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
				try {
					static_cast<void>(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)));
				} catch (const std::runtime_error &) {
					// The marker follows an actual failing native durability fence.
				}
				if (syncCalls.load(std::memory_order_relaxed) == 0U) return 113;
				graph::test::relocatable_crash::writeMarker(markerPath);
				graph::test::relocatable_crash::waitToBeKilled();
			}
			bool diskFull = false;
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[candidateOffset, metadataPoolOffset, interruptMetadata, &markerPath, &diskFull,
					 mode](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
						const bool reachesCandidate = interruptMetadata
								? absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
									  absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE
								: absoluteOffset == candidateOffset;
						if (reachesCandidate && byteCount == RSS_PAGE_SIZE) {
							if (mode == CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_DISK_FULL ||
								mode == CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_DISK_FULL) {
								diskFull = true;
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
							const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
							if (partial != 16) return partial;
							graph::test::relocatable_crash::writeMarker(markerPath);
							graph::test::relocatable_crash::waitToBeKilled();
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			try {
				static_cast<void>(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)));
			} catch (const std::system_error &) {
				if (!diskFull) throw;
			}
			if (diskFull) {
				graph::test::relocatable_crash::writeMarker(markerPath);
				graph::test::relocatable_crash::waitToBeKilled();
			}
			return 114;
		} catch (const std::exception &error) {
			graph::test::relocatable_crash::writeFailureMarker(markerPath, error.what());
			return 115;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashLargeValueTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
				("relocatable_process_crash_large_value_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	std::pair<std::shared_ptr<StorageIO>, std::unique_ptr<RelocatableMetadataStore>> reopen() const {
		auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		EXPECT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		return {io, RelocatableMetadataStore::open(io)};
	}
};

TEST_F(RelocatableProcessCrashLargeValueTest, KillDuringDataCandidateShortWriteRetainsPublishedValuesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const LargeValueManifest firstManifest = values.manifest(recovered, LargeValueId(1U));
	EXPECT_EQ(values.read(recovered, firstManifest.reference()), FIRST_VALUE);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	const LargeValueWriteResult retried = values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U));
	EXPECT_EQ(retried.reference.value_id, LargeValueId(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(values.read(published, firstManifest.reference()), FIRST_VALUE);
	EXPECT_EQ(values.read(published, retried.reference), SECOND_VALUE);
}

TEST_F(RelocatableProcessCrashLargeValueTest, KillDuringManifestCandidateShortWriteRetainsPublishedValuesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const LargeValueManifest firstManifest = values.manifest(recovered, LargeValueId(1U));
	EXPECT_EQ(values.read(recovered, firstManifest.reference()), FIRST_VALUE);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	const LargeValueWriteResult retried = values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U));
	EXPECT_EQ(retried.reference.value_id, LargeValueId(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(values.read(published, firstManifest.reference()), FIRST_VALUE);
	EXPECT_EQ(values.read(published, retried.reference), SECOND_VALUE);
}

TEST_F(RelocatableProcessCrashLargeValueTest, KillDuringMetadataCandidateShortWriteRetainsPublishedValuesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const LargeValueManifest firstManifest = values.manifest(recovered, LargeValueId(1U));
	EXPECT_EQ(values.read(recovered, firstManifest.reference()), FIRST_VALUE);
	EXPECT_THROW(static_cast<void>(values.manifest(recovered, LargeValueId(2U))), FormatException);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	const LargeValueWriteResult retried = values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U));
	EXPECT_FALSE(retried.content_deduplicated);
	EXPECT_EQ(retried.reference.value_id, LargeValueId(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(values.read(published, firstManifest.reference()), FIRST_VALUE);
	EXPECT_EQ(values.read(published, retried.reference), SECOND_VALUE);
}

TEST_F(RelocatableProcessCrashLargeValueTest, DataFenceFailureRetainsPublishedValuesAndCanRetryAfterReopen) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_DATA_FENCE_FAILURE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(values.read(recovered, values.manifest(recovered, LargeValueId(1U)).reference()), FIRST_VALUE);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	EXPECT_EQ(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)).reference.value_id, LargeValueId(2U));
}

TEST_F(RelocatableProcessCrashLargeValueTest, DataCandidateDiskFullRetainsPublishedValuesAndCanRetryAfterReopen) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_DATA_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(values.read(recovered, values.manifest(recovered, LargeValueId(1U)).reference()), FIRST_VALUE);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	EXPECT_EQ(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)).reference.value_id, LargeValueId(2U));
}

TEST_F(RelocatableProcessCrashLargeValueTest, ManifestCandidateDiskFullRetainsPublishedValuesAndCanRetryAfterReopen) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MANIFEST_CANDIDATE_DISK_FULL);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(values.read(recovered, values.manifest(recovered, LargeValueId(1U)).reference()), FIRST_VALUE);
	EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	recovered.release();
	EXPECT_EQ(values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U)).reference.value_id, LargeValueId(2U));
}

TEST_F(RelocatableProcessCrashLargeValueTest,
		KillAfterDurableLargeValueManifestEvacuationCandidateRetainsBothDirectoriesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MANIFEST_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	const LargeValueId valueId(1U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const LargeValueManifest original = values.manifest(recovered, valueId);
	const LargeValueManifestLocation originalRoute = metadata->findLargeValueManifest(recovered, valueId);
	const PageLocation originalLocation = metadata->findPageLocation(recovered, original.page_id);
	EXPECT_EQ(values.read(recovered, original.reference()), FIRST_VALUE);
	EXPECT_EQ(originalRoute.manifest_page_id, original.page_id);
	EXPECT_EQ(originalRoute.generation, original.generation);
	EXPECT_NE(originalLocation.segment_id, SegmentId(72U));
	EXPECT_EQ(originalLocation.generation, original.generation);
	recovered.release();

	const SharedPagePoolAllocation retried = values.evacuate(valueId, SegmentId(72U), LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.segment_id, SegmentId(72U));
	EXPECT_EQ(retried.location.generation, Generation(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	const LargeValueManifest relocated = values.manifest(published, valueId);
	EXPECT_EQ(values.read(published, relocated.reference()), FIRST_VALUE);
	EXPECT_EQ(metadata->findLargeValueManifest(published, valueId),
			  (LargeValueManifestLocation{valueId, original.page_id, Generation(2U)}));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).segment_id, SegmentId(72U));
	EXPECT_EQ(metadata->findPageLocation(published, original.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	published.release();

	auto [reopenedIo, reopenedMetadata] = reopen();
	LargeValueStore reopenedValues(reopenedIo, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedValues.read(reopened, reopenedValues.manifest(reopened, valueId).reference()), FIRST_VALUE);
	EXPECT_EQ(reopenedMetadata->findLargeValueManifest(reopened, valueId),
			  (LargeValueManifestLocation{valueId, original.page_id, Generation(2U)}));
}

TEST_F(RelocatableProcessCrashLargeValueTest,
		KillAfterSuperblockWriteBeforeFinalSyncRecoversOnlyACompleteLargeValueGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto [io, metadata] = reopen();
	LargeValueStore values(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const LargeValueManifest first = values.manifest(recovered, LargeValueId(1U));
	EXPECT_EQ(values.read(recovered, first.reference()), FIRST_VALUE);
	const std::optional<LargeValueManifestLocation> secondRoute =
			metadata->findLargeValueManifestOptional(recovered, LargeValueId(2U));
	const bool recoveredNewGeneration = secondRoute.has_value();
	if (recoveredNewGeneration) {
		const LargeValueManifest second = values.manifest(recovered, LargeValueId(2U));
		EXPECT_EQ(values.read(recovered, second.reference()), SECOND_VALUE);
		EXPECT_EQ(recovered.superblock().next_large_value_id, 3U);
	} else {
		EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
	}
	recovered.release();

	const LargeValueWriteResult retry = values.write(SECOND_VALUE, writeOptions(), LogSequenceNumber(3U));
	EXPECT_EQ(retry.reference.value_id, LargeValueId(2U));
	EXPECT_EQ(retry.content_deduplicated, recoveredNewGeneration);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(values.read(published, values.manifest(published, LargeValueId(1U)).reference()), FIRST_VALUE);
	EXPECT_EQ(values.read(published, values.manifest(published, LargeValueId(2U)).reference()), SECOND_VALUE);
	EXPECT_EQ(published.superblock().next_large_value_id, 3U);
	published.release();
}
