#include "storage/NativeLeaseReadTestHelper.hpp"
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
				StorageIO::ScopedNativeReadHookForTesting shortCompleteExtentRead(
						[&](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == segmentOffset && byteCount == RSS_SEGMENT_SIZE) {
								shortReads.fetch_add(1U, std::memory_order_relaxed);
								return graph::storage::testing::readWithNativeLease(fd, bytes, byteCount - RSS_PAGE_SIZE, offset);
							}
							return graph::storage::testing::readWithNativeLease(fd, bytes, byteCount, offset);
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
					if (absoluteOffset == segmentOffset) {
						if (byteCount != RSS_SEGMENT_SIZE) {
							return static_cast<pwrite_ssize_t>(-1);
						}
						extentPageReads.fetch_add(1U, std::memory_order_relaxed);
					}
					return graph::storage::testing::readWithNativeLease(fd, bytes, byteCount, offset);
				});
				EXPECT_EQ(allocator.read(snapshot, testSegmentId()).payload,
						  (std::vector<std::byte>(payload.begin(), payload.end())));
				EXPECT_EQ(allocator.read(snapshot, testSegmentId()).payload,
						  (std::vector<std::byte>(payload.begin(), payload.end())));
			}
			EXPECT_EQ(extentPageReads.load(std::memory_order_relaxed), 2U);
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}


    TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, PagedScratchReadsPreserveCrossPageContentsWithoutRetainingScanPages) {
        const file_handle_t nativeHandle = portable_open_rw(data_path_.string().c_str());
        ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
        {
            auto io = std::make_shared<StorageIO>(data_file_, nativeHandle, nativeHandle);
            auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
            PhysicalSegmentAllocator allocator(io, *metadata);
            const std::vector<std::byte> header(RSS_PAGE_SIZE + 8U, std::byte{0x71});
            const std::vector<std::byte> payload(2U * RSS_PAGE_SIZE + 19U, std::byte{0xA5});
            auto request = extentRequest(payload);
            request.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + header.size());
            request.header_extension = header;
            ASSERT_TRUE(allocator.allocate(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
            const auto snapshot = metadata->openSnapshot();
            const auto expected = allocator.read(snapshot, testSegmentId());
            const uint64_t base = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
            io->enableBufferPool(128U * RSS_PAGE_SIZE);
            size_t reads = 0U, bytesRead = 0U;
            StorageIO::ScopedNativeReadHookForTesting hook([&](file_handle_t fd, void *bytes, size_t count, pwrite_off_t offset) {
                if (static_cast<uint64_t>(offset) >= base && static_cast<uint64_t>(offset) < base + RSS_SEGMENT_SIZE) {
                    EXPECT_EQ(count, RSS_PAGE_SIZE); ++reads; bytesRead += count;
                }
                return graph::storage::testing::readWithNativeLease(fd, bytes, count, offset);
            });
            for (int iteration = 0; iteration < 2; ++iteration) {
                const auto actual = allocator.readPaged(snapshot, testSegmentId());
                EXPECT_EQ(actual.payload, expected.payload); EXPECT_EQ(actual.header_extension, expected.header_extension);
                EXPECT_EQ(SegmentEnvelopeCodec::encode(actual.envelope), SegmentEnvelopeCodec::encode(expected.envelope));
                EXPECT_EQ(actual.entry.segment_id, expected.entry.segment_id);
            }
            EXPECT_EQ(reads, 2U * RSS_SEGMENT_SIZE / RSS_PAGE_SIZE);
            EXPECT_EQ(bytesRead, 2U * RSS_SEGMENT_SIZE);
        }
        EXPECT_EQ(portable_close_rw(nativeHandle), 0);
    }

    TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, SmallPagedScratchReadsCoalescePaddingAndStillReadTheWholeTail) {
        const file_handle_t handle = portable_open_rw(data_path_.string().c_str());
        ASSERT_NE(handle, INVALID_FILE_HANDLE);
        {
            auto io = std::make_shared<StorageIO>(data_file_, handle, handle);
            auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
            PhysicalSegmentAllocator allocator(io, *metadata);
            const std::vector<std::byte> payload(64U, std::byte{0x35});
            ASSERT_TRUE(allocator.allocate(extentRequest(payload), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
            const auto snapshot = metadata->openSnapshot();
            const uint64_t base = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
            size_t reads = 0U, bytesRead = 0U;
            StorageIO::ScopedNativeReadHookForTesting hook([&](file_handle_t fd, void *bytes, size_t count, pwrite_off_t offset) {
                if (static_cast<uint64_t>(offset) >= base && static_cast<uint64_t>(offset) < base + RSS_SEGMENT_SIZE) {
                    EXPECT_EQ(static_cast<uint64_t>(offset), base + bytesRead);
                    EXPECT_LE(count, 4U * RSS_PAGE_SIZE);
                    if (reads == 0U) EXPECT_EQ(count, RSS_PAGE_SIZE);
                    ++reads; bytesRead += count;
                }
                return graph::storage::testing::readWithNativeLease(fd, bytes, count, offset);
            });
            EXPECT_EQ(allocator.readPaged(snapshot, testSegmentId()).payload, payload);
            EXPECT_EQ(reads, 9U);
            EXPECT_EQ(bytesRead, RSS_SEGMENT_SIZE);
        }
        EXPECT_EQ(portable_close_rw(handle), 0);
    }

    TEST_F(RelocatablePhysicalSegmentAllocatorTypedIOTest, PagedScratchReadsKeepChecksumMappingAndLateTruncationErrors) {
        const file_handle_t nativeHandle = portable_open_rw(data_path_.string().c_str());
        ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
        {
            auto io = std::make_shared<StorageIO>(data_file_, nativeHandle, nativeHandle);
            auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
            PhysicalSegmentAllocator allocator(io, *metadata);
            const std::array payload{std::byte{0x31}, std::byte{0x32}};
            ASSERT_TRUE(allocator.allocate(extentRequest(payload), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
            const auto snapshot = metadata->openSnapshot();
            const uint64_t base = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(40U));
            const auto observeError = [&](bool paged, int mode) -> std::optional<FormatError> {
                StorageIO::ScopedNativeReadHookForTesting hook([&](file_handle_t fd, void *bytes, size_t count, pwrite_off_t offset) {
                    const uint64_t position = static_cast<uint64_t>(offset);
                    if (mode == 2 && position >= base && position < base + RSS_SEGMENT_SIZE &&
                            position + count > base + RSS_SEGMENT_SIZE - RSS_PAGE_SIZE)
                        // portable_pread rejects a zero-byte request; inject EOF
                        // directly instead of turning truncation into an I/O error.
                        return count == RSS_PAGE_SIZE ? pwrite_ssize_t{0} :
                            graph::storage::testing::readWithNativeLease(fd, bytes, count - RSS_PAGE_SIZE, offset);
                    const auto result = graph::storage::testing::readWithNativeLease(fd, bytes, count, offset);
                    if (position == base && result == static_cast<pwrite_ssize_t>(count)) {
                        auto *image = static_cast<std::byte *>(bytes);
                        if (mode == 0 || mode == 2) image[0] ^= std::byte{1};
                        if (mode == 1) image[RSS_ENVELOPE_BYTES] ^= std::byte{1};
                        if (mode == 3) {
                            auto envelope = SegmentEnvelopeCodec::decode(std::span<const std::byte>(image, RSS_ENVELOPE_BYTES));
                            ++envelope.owner_id;
                            const auto encoded = SegmentEnvelopeCodec::encode(envelope);
                            std::copy(encoded.begin(), encoded.end(), image);
                        }
                    }
                    return result;
                });
                try { (void)(paged ? allocator.readPaged(snapshot, testSegmentId()) : allocator.read(snapshot, testSegmentId())); }
                catch (const FormatException &error) { return error.error(); }
                ADD_FAILURE() << "Expected checked physical read failure"; return std::nullopt;
            };
            for (int mode = 0; mode < 4; ++mode) {
                SCOPED_TRACE(mode);
                const auto original = observeError(false, mode), paged = observeError(true, mode);
                ASSERT_TRUE(original); EXPECT_EQ(paged, original);
                if (mode == 1) EXPECT_EQ(paged, FormatError::FERR_CHECKSUM_MISMATCH);
                if (mode == 2) EXPECT_EQ(paged, FormatError::FERR_BUFFER_TOO_SMALL);
                if (mode == 3) EXPECT_EQ(paged, FormatError::FERR_INVALID_STATE);
            }
        }
        EXPECT_EQ(portable_close_rw(nativeHandle), 0);
    }

} // namespace
