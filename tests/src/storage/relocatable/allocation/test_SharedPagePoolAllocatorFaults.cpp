/**
 * @file test_SharedPagePoolAllocatorFaults.cpp
 * @brief Write-order, recovery, and corruption tests for shared page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <source_location>
#include <span>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x6D85F371C24BA910ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	SharedPagePoolAllocationRequest newPoolRequest(uint64_t segmentId, uint64_t pageId,
												   std::span<const std::byte> payload = {}) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(segmentId);
		result.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		result.pool_owner_id = 77U;
		result.generation = Generation(1U);
		result.page_id = PageId(pageId);
		result.page_owner_id = 77U;
		result.page_owner_kind = PageOwnerKind::POK_INDEX;
		result.payload.assign(payload.begin(), payload.end());
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			if (error.error() != expected) {
				ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line()))
						<< "Unexpected FormatError: " << error.what();
			}
		}
	}

	pwrite_ssize_t failNativeWrite() {
#ifdef _WIN32
		SetLastError(ERROR_WRITE_FAULT);
#else
		errno = EIO;
#endif
		return static_cast<pwrite_ssize_t>(-1);
	}

} // namespace

class RelocatableSharedPagePoolAllocatorFaultTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<SharedPagePoolAllocator> allocator_;
	file_handle_t native_handle_ = INVALID_FILE_HANDLE;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("shared_pool_faults_" + to_string(boost::uuids::random_generator()()));
		file_ = std::make_shared<std::fstream>(
				path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		allocator_ = std::make_unique<SharedPagePoolAllocator>(io_, *metadata_);
	}

	void TearDown() override {
		allocator_.reset();
		metadata_.reset();
		io_.reset();
		if (native_handle_ != INVALID_FILE_HANDLE) {
			EXPECT_EQ(portable_close_rw(native_handle_), 0);
			native_handle_ = INVALID_FILE_HANDLE;
		}
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	void switchToNativeIo() {
		allocator_.reset();
		metadata_.reset();
		io_.reset();
		file_->flush();
		native_handle_ = portable_open_rw(path_.string().c_str());
		ASSERT_NE(native_handle_, INVALID_FILE_HANDLE);
		io_ = std::make_shared<StorageIO>(file_, native_handle_, native_handle_);
		metadata_ = RelocatableMetadataStore::open(io_);
		allocator_ = std::make_unique<SharedPagePoolAllocator>(io_, *metadata_);
	}

	void reopen() {
		allocator_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		allocator_ = std::make_unique<SharedPagePoolAllocator>(io_, *metadata_);
	}

	SharedPagePoolAllocation createPool(uint64_t segmentId, uint64_t pageId, uint64_t physicalSlot,
									 std::span<const std::byte> payload, uint64_t checkpointLsn) {
		const std::optional<SharedPagePoolAllocation> allocation = allocator_->allocateNewPoolPage(
				newPoolRequest(segmentId, pageId, payload), PhysicalSlotId(physicalSlot),
				PhysicalSlotId(physicalSlot), LogSequenceNumber(checkpointLsn));
		if (!allocation.has_value()) {
			throw std::logic_error("fault test failed to allocate a shared page pool");
		}
		return *allocation;
	}

	uint64_t poolOffset(PhysicalSlotId slot) {
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, slot);
	}

	uint8_t availableFrames(SegmentId segmentId) {
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return allocator_->availableFrameCount(metadata_->findCatalog(snapshot, segmentId));
	}

	SharedPagePoolReplacementRequest replacementRequest(std::span<const std::byte> payload) {
		SharedPagePoolReplacementRequest replacement;
		replacement.segment_id = SegmentId(31U);
		replacement.generation = Generation(1U);
		replacement.page_id = PageId(91U);
		replacement.expected_owner_lsn = LogSequenceNumber(2U);
		replacement.page_owner_id = 77U;
		replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
		replacement.payload.assign(payload.begin(), payload.end());
		return replacement;
	}

	SharedPagePoolEvacuationRequest evacuationRequest(std::span<const std::byte> payload) {
		SharedPagePoolEvacuationRequest evacuation;
		evacuation.source_segment_id = SegmentId(31U);
		evacuation.source_generation = Generation(1U);
		evacuation.destination_segment_id = SegmentId(32U);
		evacuation.destination_generation = Generation(1U);
		evacuation.page_id = PageId(91U);
		evacuation.expected_owner_lsn = LogSequenceNumber(2U);
		evacuation.page_owner_id = 77U;
		evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
		evacuation.payload.assign(payload.begin(), payload.end());
		return evacuation;
	}
};

TEST_F(RelocatableSharedPagePoolAllocatorFaultTest, ReplacementCandidateWriteFailureKeepsOldRouteAndFrameReusable) {
	const std::array oldPayload{std::byte{0x11}, std::byte{0x12}};
	const std::array newPayload{std::byte{0x21}, std::byte{0x22}};
	const SharedPagePoolAllocation pool = createPool(31U, 91U, 40U, oldPayload, 2U);
	switchToNativeIo();
	const uint64_t candidateOffset = poolOffset(pool.pool.physical_slot) + 2U * RSS_PAGE_SIZE;
	SharedPagePoolReplacementRequest replacement = replacementRequest(newPayload);
	{
		StorageIO::ScopedNativeWriteHookForTesting failCandidate(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
						return failNativeWrite();
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator_->replacePage(replacement, LogSequenceNumber(3U))),
					 std::system_error);
	}

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const SharedPagePoolContents old = allocator_->read(recovered, PageId(91U));
	EXPECT_EQ(old.location.page_index, 1U);
	EXPECT_EQ(old.payload, std::vector<std::byte>(oldPayload.begin(), oldPayload.end()));
	EXPECT_EQ(old.pool_header.allocated_frames, 1U);
	recovered.release();
	const SharedPagePoolAllocation retried =
			allocator_->replacePage(replacement, LogSequenceNumber(3U));
	EXPECT_EQ(retried.location.page_index, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorFaultTest, ReplacementControlWriteFailureReactivatesSourceAndReusesCandidate) {
	const std::array oldPayload{std::byte{0x31}, std::byte{0x32}};
	const std::array newPayload{std::byte{0x41}, std::byte{0x42}};
	const SharedPagePoolAllocation pool = createPool(31U, 91U, 40U, oldPayload, 2U);
	switchToNativeIo();
	const uint64_t controlOffset = poolOffset(pool.pool.physical_slot);
	SharedPagePoolReplacementRequest replacement = replacementRequest(newPayload);
	{
		StorageIO::ScopedNativeWriteHookForTesting failControl(
				[controlOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == controlOffset && byteCount == RSS_PAGE_SIZE) {
						return failNativeWrite();
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator_->replacePage(replacement, LogSequenceNumber(3U))),
					 std::system_error);
	}

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const SharedPagePoolContents old = allocator_->read(recovered, PageId(91U));
	EXPECT_EQ(old.location.page_index, 1U);
	EXPECT_EQ(old.owner.flags, 0U);
	EXPECT_EQ(old.payload, std::vector<std::byte>(oldPayload.begin(), oldPayload.end()));
	EXPECT_EQ(old.pool_header.allocated_frames, 1U);
	recovered.release();
	const SharedPagePoolAllocation retried =
			allocator_->replacePage(replacement, LogSequenceNumber(3U));
	EXPECT_EQ(retried.location.page_index, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorFaultTest, EvacuationCandidateWriteFailureKeepsSourceAndDestinationReusable) {
	const std::array sourcePayload{std::byte{0x51}, std::byte{0x52}};
	const std::array destinationPayload{std::byte{0x61}};
	const SharedPagePoolAllocation source = createPool(31U, 91U, 40U, sourcePayload, 2U);
	const SharedPagePoolAllocation destination = createPool(32U, 92U, 41U, destinationPayload, 3U);
	static_cast<void>(source);
	switchToNativeIo();
	const uint64_t candidateOffset = poolOffset(destination.pool.physical_slot) + 2U * RSS_PAGE_SIZE;
	SharedPagePoolEvacuationRequest evacuation = evacuationRequest(sourcePayload);
	{
		StorageIO::ScopedNativeWriteHookForTesting failCandidate(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
						return failNativeWrite();
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator_->evacuatePage(evacuation, LogSequenceNumber(4U))),
					 std::system_error);
	}

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const SharedPagePoolContents old = allocator_->read(recovered, PageId(91U));
	EXPECT_EQ(old.pool.segment_id, SegmentId(31U));
	EXPECT_EQ(old.payload, std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	EXPECT_EQ(allocator_->read(recovered, PageId(92U)).pool_header.allocated_frames, 1U);
	recovered.release();
	const SharedPagePoolAllocation retried =
			allocator_->evacuatePage(evacuation, LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.page_index, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorFaultTest, EvacuationControlWriteFailureReactivatesSourceAndReusesDestination) {
	const std::array sourcePayload{std::byte{0x71}, std::byte{0x72}};
	const std::array destinationPayload{std::byte{0x81}};
	static_cast<void>(createPool(31U, 91U, 40U, sourcePayload, 2U));
	const SharedPagePoolAllocation destination = createPool(32U, 92U, 41U, destinationPayload, 3U);
	switchToNativeIo();
	const uint64_t controlOffset = poolOffset(destination.pool.physical_slot);
	SharedPagePoolEvacuationRequest evacuation = evacuationRequest(sourcePayload);
	{
		StorageIO::ScopedNativeWriteHookForTesting failControl(
				[controlOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == controlOffset && byteCount == RSS_PAGE_SIZE) {
						return failNativeWrite();
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator_->evacuatePage(evacuation, LogSequenceNumber(4U))),
					 std::system_error);
	}

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const SharedPagePoolContents old = allocator_->read(recovered, PageId(91U));
	EXPECT_EQ(old.pool.segment_id, SegmentId(31U));
	EXPECT_EQ(old.owner.flags, 0U);
	EXPECT_EQ(old.payload, std::vector<std::byte>(sourcePayload.begin(), sourcePayload.end()));
	EXPECT_EQ(allocator_->read(recovered, PageId(92U)).pool_header.allocated_frames, 1U);
	recovered.release();
	const SharedPagePoolAllocation retried =
			allocator_->evacuatePage(evacuation, LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.page_index, 2U);
}

TEST_F(RelocatableSharedPagePoolAllocatorFaultTest, ReachableOwnerChecksumCorruptionFailsClosedOnReopen) {
	const std::array payload{std::byte{0x91}, std::byte{0x92}};
	const SharedPagePoolAllocation pool = createPool(31U, 91U, 40U, payload, 2U);
	const uint64_t ownerOffset = poolOffset(pool.pool.physical_slot) + RSS_PAGE_SIZE;
	std::byte corruption{};
	ASSERT_EQ(io_->readAt(ownerOffset + 16U, &corruption, sizeof(corruption)), sizeof(corruption));
	corruption ^= std::byte{0x01};
	io_->writeAt(ownerOffset + 16U, &corruption, sizeof(corruption));

	allocator_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	expectFormatError([&] { allocator_ = std::make_unique<SharedPagePoolAllocator>(io_, *metadata_); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}
