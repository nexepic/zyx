#include <gtest/gtest.h>
#include "graph/storage/relocatable/format/ScanBuildingClone.hpp"
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
#include <filesystem>
#include <fstream>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/storage/PreadHelper.hpp"
using namespace graph::storage;
namespace {
struct PrivateCloneFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("zyx-scan-clone-" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".db");
    file_handle_t handle = INVALID_FILE_HANDLE;
    std::shared_ptr<std::fstream> stream;
    std::unique_ptr<StorageIO> io;
    PrivateCloneFile() {
        handle = portable_create_rw_exclusive(path.string().c_str());
        if (handle == INVALID_FILE_HANDLE) throw std::runtime_error("cannot create exclusive clone fixture");
        stream = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
        io = std::make_unique<StorageIO>(stream, handle, handle);
    }
    ~PrivateCloneFile() {
        io.reset(); stream.reset();
        if (handle != INVALID_FILE_HANDLE) portable_close_rw(handle);
        std::error_code ignored; std::filesystem::remove(path, ignored);
    }
};
auto legacySourceHeader() {
    Superblock value; value.database_id = DatabaseId(71); value.generation = Generation(2);
    value.checkpoint_lsn = LogSequenceNumber(2);
    auto bytes = SuperblockCodec::encode(value);
    LittleEndianCodec::writeU16(bytes, 4, 31); LittleEndianCodec::writeU16(bytes, 6, 336);
    LittleEndianCodec::writeU32(bytes, 112, graph::utils::calculateCrc(bytes.data() + 336, bytes.size() - 336));
    LittleEndianCodec::writeU32(bytes, 116, 0);
    LittleEndianCodec::writeU32(bytes, 116, graph::utils::calculateCrc(bytes.data(), 336));
    return bytes;
}
void initializeSource(StorageIO &io) {
    std::vector<std::byte> bytes(2U * RSS_SEGMENT_SIZE);
    const auto header = legacySourceHeader();
    std::copy(header.begin(), header.end(), bytes.begin());
    std::copy(header.begin(), header.end(), bytes.begin() + RSS_SUPERBLOCK_BYTES);
    auto envelope = SegmentEnvelopeCodec::encode(SegmentEnvelope{});
    LittleEndianCodec::writeU16(envelope, 4, 5); LittleEndianCodec::writeU32(envelope, 52, 0);
    LittleEndianCodec::writeU32(envelope, 52, graph::utils::calculateCrc(envelope.data(), envelope.size()));
    std::copy(envelope.begin(), envelope.end(), bytes.begin() + RSS_SEGMENT_SIZE);
    bytes[RSS_SEGMENT_SIZE + 512] = std::byte{0x53}; // free bytes remain free
    io.writeAt(0, bytes.data(), bytes.size()); io.sync();
}
auto targetHeader(StorageIO &io) {
    std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes{};
    if (io.readAt(0, bytes.data(), bytes.size()) != bytes.size()) throw std::runtime_error("short fixture header");
    return SuperblockCodec::decode(bytes);
}
}
TEST(ScanBuildingCloneTest, CopiesIndependentImageWithoutPublishingOrChangingSource) {
    PrivateCloneFile source, target; initializeSource(*source.io);
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U);
    const auto root = scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *target.io, budget, 1024U * 1024U, [] {});
    EXPECT_EQ(root.flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_EQ(targetHeader(*target.io).flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_EQ(target.io->fileSize(), source.io->fileSize()); EXPECT_EQ(budget.used(), 0U);
    std::array<std::byte, RSS_SUPERBLOCK_BYTES> unchanged{};
    ASSERT_EQ(source.io->readAt(0, unchanged.data(), unchanged.size()), unchanged.size());
    EXPECT_EQ(unchanged, legacySourceHeader());
    std::array<std::byte, RSS_ENVELOPE_BYTES> translated{};
    ASSERT_EQ(target.io->readAt(RSS_SEGMENT_SIZE, translated.data(), translated.size()), translated.size());
    EXPECT_EQ(SegmentEnvelopeCodec::decode(translated).state, SegmentState::SGS_FREE);
    std::byte marker{}; ASSERT_EQ(target.io->readAt(RSS_SEGMENT_SIZE + 512, &marker, 1), 1U);
    EXPECT_EQ(marker, std::byte{0x53});
}
TEST(ScanBuildingCloneTest, RefusesFileAndWorkspaceBudgetsBeforeDestinationWrites) {
    PrivateCloneFile source, target; initializeSource(*source.io);
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U), tiny(16U * 1024U);
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *target.io,
        budget, 2U * RSS_SEGMENT_SIZE, [] {}), StorageFileLimitExceeded);
    EXPECT_EQ(target.io->fileSize(), 0U);
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *target.io,
        tiny, 1024U * 1024U, [] {}), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(target.io->fileSize(), 0U); EXPECT_EQ(tiny.used(), 0U);
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *source.io,
        budget, 1024U * 1024U, [] {}), std::invalid_argument);
}
TEST(ScanBuildingCloneTest, CancellationAndChangingSourceLeaveOnlyBuildingTargets) {
    PrivateCloneFile source, cancelled, changed; initializeSource(*source.io);
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U);
    size_t checks = 0;
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *cancelled.io,
        budget, 1024U * 1024U, [&] { if (++checks == 3) throw std::runtime_error("cancelled"); }), std::runtime_error);
    EXPECT_EQ(targetHeader(*cancelled.io).flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_EQ(budget.used(), 0U);
    checks = 0;
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *changed.io,
        budget, 1024U * 1024U, [&] {
            if (++checks == 5) { const std::byte zero{}; source.io->writeAt(0, &zero, 1); source.io->sync(); }
        }), FormatException);
    EXPECT_EQ(targetHeader(*changed.io).flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_EQ(budget.used(), 0U);
}
TEST(ScanBuildingCloneTest, CorruptSegmentPayloadIsNotCopiedOrPromoted) {
    PrivateCloneFile source, target; initializeSource(*source.io);
    SegmentEnvelope envelope;
    envelope.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
    envelope.state = SegmentState::SGS_SEALED; envelope.segment_id = SegmentId(1);
    envelope.owner_id = 9; envelope.generation = Generation(2); envelope.committed_lsn = LogSequenceNumber(2);
    envelope.payload_bytes = 1;
    const std::byte payload{0x53}, corrupt{0x54};
    envelope.payload_crc = graph::utils::calculateCrc(&payload, 1);
    auto header = SegmentEnvelopeCodec::encode(envelope);
    LittleEndianCodec::writeU16(header, 4, 5); LittleEndianCodec::writeU32(header, 52, 0);
    LittleEndianCodec::writeU32(header, 52, graph::utils::calculateCrc(header.data(), header.size()));
    source.io->writeAt(RSS_SEGMENT_SIZE, header.data(), header.size());
    source.io->writeAt(RSS_SEGMENT_SIZE + RSS_ENVELOPE_BYTES, &corrupt, 1);
    source.io->sync();
    graph::query::execution::MaterializedByteBudget budget(1024U * 1024U);
    EXPECT_THROW((void)scan_candidate_detail::cloneUnverifiedBuildingImage(*source.io, *target.io,
        budget, 1024U * 1024U, [] {}), FormatException);
    EXPECT_EQ(targetHeader(*target.io).flags, RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    EXPECT_EQ(target.io->fileSize(), RSS_SEGMENT_SIZE); // only the BUILDING bootstrap was copied
    EXPECT_EQ(budget.used(), 0U);
    std::byte retained{};
    ASSERT_EQ(source.io->readAt(RSS_SEGMENT_SIZE + RSS_ENVELOPE_BYTES, &retained, 1), 1U);
    EXPECT_EQ(retained, corrupt);
}
#endif
