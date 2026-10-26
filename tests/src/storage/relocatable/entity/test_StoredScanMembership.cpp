#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/storage/relocatable/entity/StoredScanMembership.hpp"
#include "graph/storage/relocatable/format/ScanBuildingImageBuilder.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace graph::storage;
using namespace graph::storage::scan_candidate_detail;

class StoredScanMembershipTest : public ::testing::Test {
protected:
    std::filesystem::path path;
    std::shared_ptr<std::fstream> file;
    std::shared_ptr<StorageIO> io;
    std::unique_ptr<RelocatableMetadataStore> metadata;
    std::shared_ptr<graph::query::execution::MaterializedByteBudget> memory;
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
            ("zyx-stored-membership-" + boost::uuids::to_string(boost::uuids::random_generator()()));
        file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file->is_open());
        io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1);
        root.checkpoint_lsn = LogSequenceNumber(1);
        if (RSS_SCAN_PATH_FORMAT_ENABLED) root.flags |= RSS_SUPERBLOCK_FLAG_SCAN_BUILDING;
        metadata = RelocatableMetadataStore::initializeNew(io, root);
        memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    }
    void TearDown() override {
        metadata.reset(); io.reset(); file.reset();
        std::error_code ignored; std::filesystem::remove(path, ignored);
    }
    void insert(uint64_t id, uint64_t ordinal, std::initializer_list<uint64_t> tokens) {
        EntityPageStore entities(io, *metadata);
        NodeEntityRecord node; node.inline_label_count = static_cast<uint8_t>(tokens.size());
        std::copy(tokens.begin(), tokens.end(), node.inline_label_ids.begin());
        const EntityPageNewPoolRequest pool{SegmentId(31 + ordinal), 901 + ordinal, Generation(1),
            PageId(101 + ordinal), PhysicalSlotId(40 + ordinal), PhysicalSlotId(40 + ordinal)};
        ASSERT_TRUE(entities.allocateNode(pool, id, node, LogSequenceNumber(2 + ordinal)).has_value());
    }
    void populate() {
        static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
            PhysicalSlotId(70), PhysicalSlotId(127), 989));
    }
};

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
TEST_F(StoredScanMembershipTest, StreamsSparseGroupsIncludingPropertylessAndUnlabeledNodesAndTheTerminalId) {
    insert(65, 0, {11}); insert(1, 1, {7, 11}); insert(64, 2, {}); insert(UINT64_MAX, 3, {});
    populate();
    auto pin = metadata->openSnapshot();
    std::vector<ScanBlockMembership> records;
    io->resetStats();
    const auto counts = streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX, [&](const auto &record) {
        records.push_back(record); return true;
    });
    ASSERT_EQ(records.size(), 3U);
    EXPECT_TRUE(counts.exhausted); EXPECT_EQ(counts.nodes, 4U); EXPECT_EQ(counts.groups, 3U);
    EXPECT_EQ(records[0].group_id, 1U); EXPECT_EQ(records[0].live, UINT64_C(1) | (UINT64_C(1) << 63U));
    EXPECT_EQ(records[0].select(std::array<uint64_t, 1>{7}), 1U);
    EXPECT_EQ(records[1].group_id, 2U); EXPECT_EQ(records[1].live, 1U);
    EXPECT_EQ(records[2].group_id, ScanMembershipDirectoryCodec::MAX_GROUP);
    EXPECT_EQ(records[2].live, UINT64_C(1) << 62U);
    EXPECT_EQ(records[2].label_count, 0U);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(memory->used(), 0U);
}


TEST_F(StoredScanMembershipTest, PackedMembershipsAcquireOneAuthenticatedPagePerStream) {
    insert(1, 0, {7}); insert(65, 1, {11}); populate(); auto pin = metadata->openSnapshot();
    struct Counters { int64_t reads = 0, reuses = 0; } counts;
    graph::debug::PerfTrace::ScopedObserver observer(&counts,
        [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
            if (duration) return;
            auto &state = *static_cast<Counters *>(owner);
            if (key == "native_graph.scan_membership.page_acquisitions") state.reads += value;
            if (key == "native_graph.scan_membership.page_reuses") state.reuses += value;
        });
    const auto result = streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX, [](const auto &) { return true; });
    EXPECT_EQ(result.groups, 2U); EXPECT_TRUE(result.exhausted);
    EXPECT_EQ(counts.reads, 1); EXPECT_EQ(counts.reuses, 1); EXPECT_EQ(memory->used(), 0U);
}


TEST_F(StoredScanMembershipTest, PackedStreamReacquiresAfterACallbackIoMutation) {
    insert(1, 0, {7}); insert(65, 1, {11}); populate(); auto pin = metadata->openSnapshot();
    int64_t acquisitions = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&acquisitions,
        [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
            if (!duration && key == "native_graph.scan_membership.page_acquisitions")
                *static_cast<int64_t *>(owner) += value;
        });
    size_t calls = 0;
    const auto result = streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX, [&](const auto &) {
        if (++calls == 1U) {
            std::array<std::byte, 8> unchanged{};
            EXPECT_EQ(io->readAt(0, unchanged.data(), unchanged.size()), unchanged.size());
            io->writeAt(0, unchanged.data(), unchanged.size());
        }
        return true;
    });
    EXPECT_TRUE(result.exhausted); EXPECT_EQ(calls, 2U); EXPECT_EQ(acquisitions, 2);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(StoredScanMembershipTest, EmptyStorageAndExplicitPrefixStopsHaveDistinctCompletionResults) {
    auto pin = metadata->openSnapshot();
    size_t calls = 0;
    auto counts = streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [&](const auto &) { ++calls; return true; });
    EXPECT_TRUE(counts.exhausted); EXPECT_EQ(calls, 0U); pin.release();
    insert(1, 0, {}); insert(65, 1, {}); populate(); pin = metadata->openSnapshot();
    counts = streamStoredMembership(io, *metadata, pin, memory, 1U,
        [&](const auto &) { ++calls; return true; });
    EXPECT_FALSE(counts.exhausted); EXPECT_EQ(counts.groups, 1U); EXPECT_EQ(calls, 1U);
    calls = 0;
    counts = streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [&](const auto &) { ++calls; return false; });
    EXPECT_FALSE(counts.exhausted); EXPECT_EQ(calls, 1U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(StoredScanMembershipTest, CancellationAfterDeliveryDoesNotReplayThePrefix) {
    insert(1, 0, {}); insert(65, 1, {}); populate(); auto pin = metadata->openSnapshot();
    struct Cancel final : CancellationCheckpoint {
        bool stop = false;
        void check() const override { if (stop) throw std::runtime_error("cancel stored membership"); }
    } cancel;
    size_t calls = 0;
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [&](const auto &) { ++calls; cancel.stop = true; return true; }, &cancel)), std::runtime_error);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata->activeSnapshotCount(), 1U); pin.release();
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(StoredScanMembershipTest, ForeignAndReleasedPinsCannotContinueAStoredStream) {
    insert(1, 0, {}); insert(65, 1, {}); populate();
    auto foreign = RelocatableMetadataStore::openReadOnly(io);
    auto pin = foreign->openSnapshot(); io->resetStats();
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [](const auto &) { return true; })), std::invalid_argument);
    EXPECT_EQ(io->stats().read_calls, 0U); pin.release();
    pin = metadata->openSnapshot(); size_t calls = 0;
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [&](const auto &) { ++calls; pin.release(); return true; })), std::invalid_argument);
    EXPECT_EQ(calls, 1U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(StoredScanMembershipTest, WorkspaceRefusalPrecedesAnyStorageReadOrConsumerCall) {
    insert(1, 0, {}); populate(); auto pin = metadata->openSnapshot();
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(256U);
    size_t calls = 0; io->resetStats();
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, tiny, SIZE_MAX,
        [&](const auto &) { ++calls; return true; })), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(calls, 0U); EXPECT_EQ(tiny->used(), 0U);
}

TEST_F(StoredScanMembershipTest, FullExhaustionRejectsAValidCrcButIncorrectDescriptorCount) {
    insert(1, 0, {}); insert(65, 1, {}); populate();
    const auto root = metadata->openSnapshot().superblock();
    MetadataPageFile pages(io, root);
    auto descriptor = pages.readHybridPropertyDescriptor(root.hybrid_property_descriptor_root, root);
    ++descriptor.descriptor.scan_live_node_count;
    const auto bytes = HybridPropertyDescriptorCodec::encode(descriptor);
    const auto reference = root.hybrid_property_descriptor_root;
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(root.data_region_offset, reference.physical_slot) +
        uint64_t(reference.frame_index) * RSS_PAGE_SIZE;
    io->writeAt(offset, bytes.data(), bytes.size());
    auto pin = metadata->openSnapshot();
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [](const auto &) { return true; })), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
#else
TEST_F(StoredScanMembershipTest, OrdinaryFormatsRejectTheStreamBeforeReadingStorage) {
    auto pin = metadata->openSnapshot(); io->resetStats();
    EXPECT_THROW(static_cast<void>(streamStoredMembership(io, *metadata, pin, memory, SIZE_MAX,
        [](const auto &) { return true; })), FormatException);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
}
#endif
