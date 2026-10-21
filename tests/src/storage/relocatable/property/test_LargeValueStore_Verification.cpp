/**
 * @file test_LargeValueStore_Verification.cpp
 * @brief Bounded complete-content authentication, independent of owning reads.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace {
LargeValueWriteOptions verificationOptions() {
    LargeValueWriteOptions options;
    options.first_data_slot = PhysicalSlotId(40); options.last_data_slot = PhysicalSlotId(80);
    options.first_manifest_slot = PhysicalSlotId(81); options.last_manifest_slot = PhysicalSlotId(90);
    options.kind = LargeValueKind::LVK_BYTES; options.media_type = "application/test-verification";
    return options;
}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueVerificationAuthenticatesBeyondWorkspaceAndRetainsOnlyRootCredit) {
    auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
    LargeValueStore store(io, *metadata);
    std::vector<std::byte> input((RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS + 1U) *
        size_t(RSS_LARGE_VALUE_EXTENT_32K_BYTES) + 73U, std::byte{0x47});
    const auto written = store.write(input, verificationOptions(), LogSequenceNumber(2));
    ASSERT_FALSE(written.manifest.children.empty());
    const auto pin = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(128 * 1024);
    {
        graph::utils::OwnedHeapReservation credit(memory);
        const auto admit = [&](size_t count) { credit.set(count); };
        EXPECT_THROW(static_cast<void>(store.read(pin, written.reference, std::ref(admit))),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    {
        graph::utils::OwnedHeapReservation credit(memory);
        size_t peak = 0;
        const auto admit = [&](size_t count) { credit.set(count); peak = (std::max)(peak, count); };
        const auto manifest = store.verifyContent(pin, written.reference, std::ref(admit));
        EXPECT_EQ(manifest.reference(), written.reference);
        EXPECT_LT(peak, input.size());
        EXPECT_EQ(credit.bytes(), manifest.media_type.capacity() + manifest.chunks.capacity() * sizeof(LargeValueChunk) +
            manifest.children.capacity() * sizeof(LargeValueManifestChild));
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueVerificationEmptyCompressedCancellationAndQuotaRetry) {
    auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
    LargeValueStore store(io, *metadata);
    auto options = verificationOptions();
    const auto empty = store.write({}, options, LogSequenceNumber(2));
    { const auto pin = metadata->openSnapshot(); EXPECT_EQ(store.verifyContent(pin, empty.reference).reference(), empty.reference); }
    options.compression.codec = LargeValueCodec::LVC_LZ4;
    options.compression.allow_raw_fallback = false; options.compression.minimum_savings_percent = 1;
    const auto written = store.write(std::vector<std::byte>(256 * 1024, std::byte{0x51}), options, LogSequenceNumber(3));
    const auto pin = metadata->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io->dataMutationEpoch();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(128 * 1024);
    {
        graph::utils::OwnedHeapReservation credit(memory);
        const auto admit = [&](size_t count) { credit.set(count); };
        size_t checks = 0;
        EXPECT_THROW(static_cast<void>(store.verifyContent(pin, written.reference, std::ref(admit), [&] {
            if (++checks == 3) throw std::runtime_error("cancel full-content verification");
        })), std::runtime_error);
    }
    EXPECT_EQ(memory->used(), 0U);
    {
        graph::utils::OwnedHeapReservation credit(memory);
        const auto reject = [&](size_t count) {
            if (count > 8 * 1024) throw std::runtime_error("deny range buffer");
            credit.set(count);
        };
        EXPECT_THROW(static_cast<void>(store.verifyContent(pin, written.reference, std::ref(reject))), std::runtime_error);
    }
    EXPECT_EQ(memory->used(), 0U);
    {
        graph::utils::OwnedHeapReservation credit(memory);
        const auto admit = [&](size_t count) { credit.set(count); };
        EXPECT_EQ(store.verifyContent(pin, written.reference, std::ref(admit)).reference(), written.reference);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    EXPECT_EQ(io->dataMutationEpoch(), epoch);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueVerificationRejectsValidChunkChecksumsWithWrongFullContent) {
    auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
    LargeValueStore store(io, *metadata);
    const auto written = store.write(std::vector<std::byte>(73, std::byte{0x31}), verificationOptions(), LogSequenceNumber(2));
    const auto pin = metadata->openSnapshot();
    SharedPagePoolAllocator pools(io, *metadata);
    const auto dataId = written.manifest.chunks.front().first_data_page_id;
    const auto data = pools.read(pin, dataId);
    auto changed = LargeValueDataFrame::open(data.owner, data.payload).page();
    changed.payload.front() ^= std::byte{0xff};
    const auto frame = LargeValueDataFrame::create(data.owner, changed);
    const auto overwrite = [&](const SharedPagePoolContents &contents, std::span<const std::byte> bytes) {
        const auto position = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset,
            contents.pool.physical_slot) + uint64_t(contents.location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
        io->writeAt(position, bytes.data(), bytes.size());
    };
    overwrite(data, frame.serialize());
    const auto manifestPage = pools.read(pin, written.manifest.page_id);
    auto manifest = LargeValueManifestPage::open(manifestPage.owner, manifestPage.payload).manifest();
    manifest.chunks.front().stored_crc = manifest.chunks.front().uncompressed_crc =
        graph::utils::calculateCrc(changed.payload.data(), changed.payload.size());
    const auto replacement = LargeValueManifestPage::create(manifestPage.owner, manifest);
    overwrite(manifestPage, replacement.serialize()); io->sync();
    expectFormatError([&] { static_cast<void>(store.verifyContent(pin, written.reference)); }, FormatError::FERR_CHECKSUM_MISMATCH);
}
