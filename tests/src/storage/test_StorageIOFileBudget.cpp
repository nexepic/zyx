#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"

using namespace graph::storage;
namespace {
class StorageIOFileBudgetTest : public ::testing::Test {
protected:
    std::filesystem::path path;
    std::shared_ptr<std::fstream> stream;
    std::shared_ptr<StorageIO> io;
    file_handle_t write_fd = INVALID_FILE_HANDLE, read_fd = INVALID_FILE_HANDLE;
    void SetUp() override {
        path = std::filesystem::temp_directory_path() / ("zyx-file-budget-" + to_string(boost::uuids::random_generator()()));
        stream = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream->is_open());
    }
    void configure(uint64_t limit, bool native) {
        if (native) {
            write_fd = portable_open_rw(path.string().c_str());
            read_fd = portable_open(path.string().c_str(), O_RDONLY);
            ASSERT_NE(write_fd, INVALID_FILE_HANDLE); ASSERT_NE(read_fd, INVALID_FILE_HANDLE);
        }
        io = std::make_shared<StorageIO>(stream, write_fd, read_fd, StorageIOAccessMode::SIOAM_READ_WRITE, limit);
    }
    void TearDown() override {
        io.reset();
        static_cast<void>(portable_close_rw(write_fd)); static_cast<void>(portable_close(read_fd));
        stream.reset(); std::error_code error; std::filesystem::remove(path, error);
    }
    template<class Operation> void expectQuotaDenied(Operation operation) {
        try { operation(); FAIL() << "write beyond the file quota succeeded"; }
        catch (const std::system_error &error) { EXPECT_EQ(error.code(), std::make_error_code(std::errc::no_space_on_device)); }
    }
};

TEST_F(StorageIOFileBudgetTest, ExactBoundaryAndDeniedGrowthPreserveBytesForStreamAndNativeIO) {
    for (bool native : {false, true}) {
        SCOPED_TRACE(native); configure(8192U, native);
        const std::array<std::byte, 2U> marker{std::byte{71}, std::byte{72}};
        io->writeAt(8190U, marker.data(), marker.size()); io->sync();
        ASSERT_EQ(io->fileSize(), 8192U);
        const auto before = io->stats();
        expectQuotaDenied([&] { io->writeAt(8191U, marker.data(), marker.size()); });
        expectQuotaDenied([&] { (void)io->append(marker.data(), marker.size()); });
        expectQuotaDenied([&] { (void)io->reserveAppendSpace(1U); });
        EXPECT_EQ(io->fileSize(), 8192U); EXPECT_EQ(io->stats(), before);
        std::array<std::byte, 2U> actual{};
        ASSERT_EQ(io->readAt(8190U, actual.data(), actual.size()), actual.size()); EXPECT_EQ(actual, marker);
        io.reset();
        static_cast<void>(portable_close_rw(write_fd)); static_cast<void>(portable_close(read_fd));
        write_fd = read_fd = INVALID_FILE_HANDLE;
    }
}

TEST_F(StorageIOFileBudgetTest, TypedAndControlDenialsPrecedeDirtyCacheFlushOrPartialBatchWrites) {
    configure(8192U, true);
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{}; image.fill(std::byte{3});
    io->writeAt({0U, 1U}, image.data(), image.size());
    io->writeAt({RSS_BUFFER_POOL_PAGE_BYTES, 1U}, image.data(), image.size());
    io->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
    image.fill(std::byte{17}); io->writeAt({0U, 1U}, image.data(), image.size());
    const auto before = io->stats(); const auto cache = io->bufferPoolStats();
    std::array<std::byte, 2U> marker{std::byte{71}, std::byte{72}};
    std::vector<std::byte> twoPages(2U * RSS_BUFFER_POOL_PAGE_BYTES, std::byte{88});
    expectQuotaDenied([&] { io->writeAt({8192U, 1U}, image.data(), image.size()); });
    expectQuotaDenied([&] { io->writePages({4096U, 1U}, twoPages.data(), twoPages.size()); });
    expectQuotaDenied([&] { io->writePagesThrough({4096U, 1U}, twoPages.data(), twoPages.size()); });
    expectQuotaDenied([&] { io->writeControlRecord({8192U, 1U}, 0U, marker); });
    EXPECT_EQ(io->stats(), before);
    EXPECT_EQ(io->bufferPoolStats().resident_bytes, cache.resident_bytes);
    EXPECT_EQ(io->bufferPoolStats().dirty_bytes, cache.dirty_bytes);
    EXPECT_EQ(io->bufferPoolStats().evictions, cache.evictions);
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> actual{};
    EXPECT_EQ(io->readAt({0U, 1U}, actual.data(), actual.size()), actual.size()); EXPECT_EQ(actual, image);
    EXPECT_EQ(io->fileSize(), 8192U);
}

TEST_F(StorageIOFileBudgetTest, RejectsZeroLimitAndKeepsReadonlyAdmissionIndependentOfFileLength) {
    EXPECT_THROW((void)StorageIO(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE,
        StorageIOAccessMode::SIOAM_READ_WRITE, 0U), std::invalid_argument);
    configure(8192U, false);
    const std::byte marker{42}; io->writeAt(8191U, &marker, 1U); io->sync(); io.reset();
    io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE,
        StorageIOAccessMode::SIOAM_READ_ONLY, 1U);
    std::byte actual{}; EXPECT_EQ(io->readAt(8191U, &actual, 1U), 1U); EXPECT_EQ(actual, marker);
    EXPECT_THROW(io->writeAt(0U, &marker, 1U), std::logic_error);
}
}
