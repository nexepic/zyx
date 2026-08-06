/**
 * @file test_PhysicalSegmentAllocatorTypedIO.cpp
 * @brief Complete-page I/O tests for relocatable whole-segment extents.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <source_location>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t TEST_SEGMENT_ORDINAL = 1U;

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1122334455667788ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	SegmentId testSegmentId() { return SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + TEST_SEGMENT_ORDINAL - 1U); }

	PhysicalSegmentAllocationRequest extentRequest(std::span<const std::byte> payload) {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = testSegmentId();
		result.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
		result.owner_id = 17U;
		result.generation = Generation(1U);
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

	class RelocatablePhysicalSegmentAllocatorTypedIOTest : public ::testing::Test {
	protected:
		std::filesystem::path data_path_;
		std::shared_ptr<std::fstream> data_file_;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			data_path_ = std::filesystem::temp_directory_path() /
						 ("relocatable_physical_segment_typed_io_" + to_string(uuid) + ".dat");
			data_file_ = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary |
																			std::ios::trunc);
			ASSERT_TRUE(data_file_->is_open());
		}

		void TearDown() override {
			if (data_file_ && data_file_->is_open()) {
				data_file_->close();
			}
			data_file_.reset();
			std::error_code error;
			std::filesystem::remove(data_path_, error);
		}
	};

	TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, WritesOnlyCompleteGenerationTaggedExtentPages) {
		const file_handle_t nativeHandle = portable_open_rw(data_path_.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto io = std::make_shared<StorageIO>(data_file_, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			MetadataSnapshotPin initial = metadata->openSnapshot();
			const uint64_t segmentOffset = PhysicalSegmentAllocator::checkedSlotOffset(
					initial.superblock().data_region_offset, PhysicalSlotId(40U));
			initial.release();
			PhysicalSegmentAllocator allocator(io, *metadata);

			std::atomic<uint32_t> completePageWrites{0U};
			std::atomic<uint32_t> reservationWrites{0U};
			std::atomic<bool> malformedWrite{false};
			std::atomic<uint64_t> observedPages{0U};
			const std::array payload = {std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
			{
				StorageIO::ScopedNativeWriteHookForTesting observeExtentWrites(
						[&](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
							if (absoluteOffset >= segmentOffset && absoluteOffset < segmentOffset + RSS_SEGMENT_SIZE) {
								if (absoluteOffset == segmentOffset + RSS_SEGMENT_SIZE - 1U && byteCount == 1U) {
									reservationWrites.fetch_add(1U, std::memory_order_relaxed);
								} else if (byteCount == RSS_PAGE_SIZE &&
										   (absoluteOffset - segmentOffset) % RSS_PAGE_SIZE == 0U) {
									const uint64_t page = (absoluteOffset - segmentOffset) / RSS_PAGE_SIZE;
									observedPages.fetch_or(UINT64_C(1) << page, std::memory_order_relaxed);
									completePageWrites.fetch_add(1U, std::memory_order_relaxed);
								} else {
									malformedWrite.store(true, std::memory_order_relaxed);
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				ASSERT_TRUE(allocator
									.allocate(extentRequest(payload), PhysicalSlotId(40U), PhysicalSlotId(40U),
											  LogSequenceNumber(2U))
									.has_value());
			}

			EXPECT_FALSE(malformedWrite.load(std::memory_order_relaxed));
			EXPECT_LE(reservationWrites.load(std::memory_order_relaxed), 1U);
			EXPECT_EQ(completePageWrites.load(std::memory_order_relaxed), RSS_SEGMENT_SIZE / RSS_PAGE_SIZE);
			EXPECT_EQ(observedPages.load(std::memory_order_relaxed),
					  (UINT64_C(1) << (RSS_SEGMENT_SIZE / RSS_PAGE_SIZE)) - 1U);
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}

	TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, RejectsAShortReadInAnUnusedTrailingExtentPage) {
		const file_handle_t nativeHandle = portable_open_rw(data_path_.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto io = std::make_shared<StorageIO>(data_file_, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			PhysicalSegmentAllocator allocator(io, *metadata);
			const std::array payload = {std::byte{0x31}};
			ASSERT_TRUE(allocator
								.allocate(extentRequest(payload), PhysicalSlotId(40U), PhysicalSlotId(40U),
										  LogSequenceNumber(2U))
								.has_value());
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint64_t segmentOffset = PhysicalSegmentAllocator::checkedSlotOffset(
					snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
			std::atomic<uint32_t> shortReads{0U};
			{
				StorageIO::ScopedNativeReadHookForTesting shortSecondExtentPage(
						[&](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == segmentOffset + RSS_PAGE_SIZE &&
								byteCount == RSS_PAGE_SIZE) {
								shortReads.fetch_add(1U, std::memory_order_relaxed);
								return portable_pread(fd, bytes, byteCount - 1U, offset);
							}
							return portable_pread(fd, bytes, byteCount, offset);
						});
				expectFormatError([&] { static_cast<void>(allocator.read(snapshot, testSegmentId())); },
								  FormatError::FERR_BUFFER_TOO_SMALL);
			}
			EXPECT_EQ(shortReads.load(std::memory_order_relaxed), 1U);
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}

	TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, SequentialExtentReadsDoNotRetainScanPages) {
		const file_handle_t nativeHandle = portable_open_rw(data_path_.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto io = std::make_shared<StorageIO>(data_file_, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			PhysicalSegmentAllocator allocator(io, *metadata);
			const std::array payload = {std::byte{0x41}, std::byte{0x42}};
			ASSERT_TRUE(allocator
								.allocate(extentRequest(payload), PhysicalSlotId(40U), PhysicalSlotId(40U),
										  LogSequenceNumber(2U))
								.has_value());
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint64_t segmentOffset = PhysicalSegmentAllocator::checkedSlotOffset(
					snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
			io->enableBufferPool(128U * RSS_PAGE_SIZE);
			std::atomic<uint32_t> extentPageReads{0U};
			{
				StorageIO::ScopedNativeReadHookForTesting countExtentReads([&](file_handle_t fd, void *bytes,
																			   size_t byteCount, pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (absoluteOffset >= segmentOffset && absoluteOffset < segmentOffset + RSS_SEGMENT_SIZE) {
						if (byteCount != RSS_PAGE_SIZE || (absoluteOffset - segmentOffset) % RSS_PAGE_SIZE != 0U) {
							return static_cast<pwrite_ssize_t>(-1);
						}
						extentPageReads.fetch_add(1U, std::memory_order_relaxed);
					}
					return portable_pread(fd, bytes, byteCount, offset);
				});
				EXPECT_EQ(allocator.read(snapshot, testSegmentId()).payload,
						  (std::vector<std::byte>(payload.begin(), payload.end())));
				EXPECT_EQ(allocator.read(snapshot, testSegmentId()).payload,
						  (std::vector<std::byte>(payload.begin(), payload.end())));
			}
			EXPECT_EQ(extentPageReads.load(std::memory_order_relaxed), 2U * (RSS_SEGMENT_SIZE / RSS_PAGE_SIZE));
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}

} // namespace
