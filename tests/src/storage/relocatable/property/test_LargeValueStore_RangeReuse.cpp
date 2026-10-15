/** @file test_LargeValueStore_RangeReuse.cpp
 * @brief Local chunk reuse, admission, mutation and identity contracts.
 */
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <array>
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"

namespace {
LargeValueWriteOptions options(LargeValueCodec codec = LargeValueCodec::LVC_RAW) {
 LargeValueWriteOptions result;
 result.first_data_slot = PhysicalSlotId(40);
 result.last_data_slot = PhysicalSlotId(140);
 result.first_manifest_slot = PhysicalSlotId(141);
 result.last_manifest_slot = PhysicalSlotId(170);
 result.kind = LargeValueKind::LVK_BYTES;
 result.media_type = "application/octet-stream";
 result.compression.codec = codec;
 result.compression.allow_raw_fallback = false;
 result.extent_class = LargeValueExtentClass::LVEC_16K;
 return result;
}
struct Values {
 bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 Values() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true); }
 ~Values() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); }
};
std::vector<std::byte> slice(const std::vector<std::byte> &input, size_t offset, size_t length) {
 return {input.begin() + offset, input.begin() + offset + length};
}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRangeReuseValidatesRawAndCompressedChunksOnce) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 uint64_t lsn = 2;
 for (const auto codec : {LargeValueCodec::LVC_RAW, LargeValueCodec::LVC_LZ4, LargeValueCodec::LVC_ZSTD}) {
  const std::vector<std::byte> input(8192, static_cast<std::byte>(codec));
  const auto written = store.write(input, options(codec), LogSequenceNumber(lsn++));
  auto snapshot = metadata->openSnapshot();
  const auto manifest = store.manifest(snapshot, written.reference);
  ASSERT_EQ(manifest.chunks.size(), 1U);
  EXPECT_EQ(manifest.chunks.front().codec, codec);
  LargeValueStore::RangeReadCache cache;
  Values values;
  for (const size_t offset : {0U, 128U, 512U}) {
   size_t admitted = 0;
   const auto result = store.readRangeFromManifestCached(snapshot, manifest, offset, 32, cache,
    [&](size_t bytes) { admitted = bytes; });
   EXPECT_EQ(result, slice(input, offset, 32));
   EXPECT_EQ(admitted, result.capacity() + cache.retainedBytes());
   EXPECT_LE(cache.retainedBytes(), RSS_LARGE_VALUE_EXTENT_128K_BYTES);
  }
  const auto trace = graph::debug::PerfTrace::snapshotAndReset();
  EXPECT_EQ(trace.at("native_graph.read.large_value_chunks_read").totalValue, 1);
  EXPECT_EQ(trace.at("native_graph.read.large_value_range_chunk_reuse").totalValue, 2);
  auto forged = manifest;
  forged.chunks.front().uncompressed_crc ^= 1U;
  expectFormatError([&] { static_cast<void>(store.readRangeFromManifestCached(snapshot, forged, 0, 1, cache)); },
   FormatError::FERR_CHECKSUM_MISMATCH);
  EXPECT_EQ(cache.retainedBytes(), 0U);
 }
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRangeReuseAdmitsRetainedBufferAndRetriesAfterFailure) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 const std::vector<std::byte> input(8192, std::byte{0x42});
 const auto written = store.write(input, options(), LogSequenceNumber(2));
 auto snapshot = metadata->openSnapshot();
 const auto manifest = store.manifest(snapshot, written.reference);
 LargeValueStore::RangeReadCache cache;
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, 0, 16, cache), slice(input, 0, 16));
 EXPECT_THROW(static_cast<void>(store.readRangeFromManifestCached(snapshot, manifest, 128, 16, cache,
  [&](size_t bytes) { if (bytes > 16) throw std::length_error("retained chunk rejected"); })), std::length_error);
 EXPECT_EQ(cache.retainedBytes(), 0U);
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, 128, 16, cache), slice(input, 128, 16));
 snapshot.release();
 size_t admissionCalls = 0;
 expectFormatError([&] { static_cast<void>(store.readRangeFromManifestCached(snapshot, manifest, 0, 1, cache,
  [&](size_t) { ++admissionCalls; })); }, FormatError::FERR_INVALID_STATE);
 EXPECT_EQ(admissionCalls, 0U);
 EXPECT_EQ(cache.retainedBytes(), 0U);
 auto retry = metadata->openSnapshot();
 EXPECT_EQ(store.readRangeFromManifestCached(retry, manifest, 128, 16, cache), slice(input, 128, 16));
 LargeValueStore otherStore(io, *metadata);
 Values values;
 EXPECT_EQ(otherStore.readRangeFromManifestCached(retry, manifest, 256, 16, cache), slice(input, 256, 16));
 EXPECT_EQ(graph::debug::PerfTrace::snapshotAndReset().at("native_graph.read.large_value_chunks_read").totalValue, 1);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRangeReuseEvictsAtChunkBoundaryAndAfterStorageMutation) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 std::vector<std::byte> input(40000);
 for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::byte>(i % 251U);
 const auto written = store.write(input, options(), LogSequenceNumber(2));
 auto snapshot = metadata->openSnapshot();
 const auto manifest = store.manifest(snapshot, written.reference);
 ASSERT_GT(manifest.chunks.size(), 1U);
 const size_t boundary = manifest.chunks.front().uncompressed_bytes;
 LargeValueStore::RangeReadCache cache;
 Values values;
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, boundary - 16, 32, cache), slice(input, boundary - 16, 32));
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, boundary + 32, 16, cache), slice(input, boundary + 32, 16));
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 EXPECT_EQ(trace.at("native_graph.read.large_value_chunks_read").totalValue, 2);
 EXPECT_EQ(trace.at("native_graph.read.large_value_range_chunk_reuse").totalValue, 1);
 const auto pageId = manifest.chunks[1].first_data_page_id;
 const auto location = metadata->findPageLocation(snapshot, pageId);
 const auto pool = metadata->findCatalog(snapshot, location.segment_id);
 const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, pool.physical_slot)
  + static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
 std::array<std::byte, RSS_PAGE_SIZE> bytes{};
 ASSERT_EQ(io->readAt({offset, location.generation.value()}, bytes.data(), bytes.size()), bytes.size());
 const auto saved = bytes;
 bytes[RSS_PAGE_OWNER_HEADER_BYTES + RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES] ^= std::byte{1};
 io->writeAt({offset, location.generation.value()}, bytes.data(), bytes.size());
 expectFormatError([&] { static_cast<void>(store.readRangeFromManifestCached(snapshot, manifest, boundary + 32, 16, cache)); },
  FormatError::FERR_CHECKSUM_MISMATCH);
 EXPECT_EQ(cache.retainedBytes(), 0U);
 io->writeAt({offset, location.generation.value()}, saved.data(), saved.size());
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, boundary + 32, 16, cache), slice(input, boundary + 32, 16));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRangeReuseKeepsChildValidationAndOldSnapshotAfterPublication) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 const std::vector<std::byte> input((RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES,
  std::byte{0x37});
 const auto written = store.write(input, options(), LogSequenceNumber(2));
 auto before = metadata->openSnapshot();
 const auto root = store.manifest(before, written.reference);
 ASSERT_FALSE(root.isLeaf());
 LargeValueStore::RangeReadCache cache;
 const size_t offset = input.size() - 64U;
 size_t peak = 0;
 EXPECT_EQ(store.readRangeFromManifestCached(before, root, offset, 16, cache,
  [&](size_t bytes) { peak = std::max(peak, bytes); }), slice(input, offset, 16));
 EXPECT_GT(peak, cache.retainedBytes());
 Values values;
 EXPECT_EQ(store.readRangeFromManifestCached(before, root, offset + 16, 16, cache), slice(input, offset + 16, 16));
 auto reused = graph::debug::PerfTrace::snapshotAndReset();
 EXPECT_EQ(reused.at("native_graph.read.large_value_range_chunk_reuse").totalValue, 1);
 const std::vector<std::byte> next(8192, std::byte{0x79});
 const auto second = store.write(next, options(), LogSequenceNumber(3));
 // Publication changes the IO epoch but cannot change the old pin's data.
 graph::debug::PerfTrace::reset();
 EXPECT_EQ(store.readRangeFromManifestCached(before, root, offset, 16, cache), slice(input, offset, 16));
 auto reloaded = graph::debug::PerfTrace::snapshotAndReset();
 EXPECT_EQ(reloaded.at("native_graph.read.large_value_chunks_read").totalValue, 1);
 auto current = metadata->openSnapshot();
 const auto nextRoot = store.manifest(current, second.reference);
 EXPECT_EQ(store.readRangeFromManifestCached(current, nextRoot, 0, 16, cache), slice(next, 0, 16));
}

TEST_F(RelocatableMetadataStoreTest, PrivateChunkCacheRetainsItsAllocationOwnerAfterDecodeScopeEnds) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 const std::vector<std::byte> input(8192U, std::byte{0x59});
 const auto written = store.write(input, options(), LogSequenceNumber(2));
 auto snapshot = metadata->openSnapshot();
 const auto manifest = store.manifest(snapshot, written.reference);
 LargeValueStore::RangeReadCache cache;
 auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(262144U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(budget);
  const auto result = store.readRangeFromManifestCached(snapshot, manifest, 0U, 16U, cache);
  EXPECT_EQ(result, slice(input, 0U, 16U));
  EXPECT_GE(budget->used(), result.capacity() + cache.retainedBytes());
 }
 EXPECT_EQ(budget->used(), cache.retainedBytes());
 EXPECT_EQ(cache.retainedBytes(), input.size());
 cache.clear();
 EXPECT_EQ(budget->used(), 0U);
 auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(small);
  EXPECT_THROW(static_cast<void>(store.readRangeFromManifestCached(snapshot, manifest, 0U, 16U, cache)),
   graph::query::execution::MaterializedByteLimitExceeded);
 }
 EXPECT_EQ(small->used(), 0U); EXPECT_EQ(cache.retainedBytes(), 0U);
 EXPECT_EQ(store.readRangeFromManifestCached(snapshot, manifest, 0U, 16U, cache), slice(input, 0U, 16U));
}

TEST_F(RelocatableMetadataStoreTest, PrivateSequentialChunkWorkspaceDoesNotAccumulateReleasedChunkBuffers) {
 auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
 LargeValueStore store(io, *metadata);
 const std::vector<std::byte> input(100000U, std::byte{0x59});
 const auto written = store.write(input, options(), LogSequenceNumber(2));
 auto snapshot = metadata->openSnapshot();
 const auto manifest = store.manifest(snapshot, written.reference);
 ASSERT_GT(manifest.chunks.size(), 4U);
 auto budget = std::make_shared<graph::query::execution::MaterializedByteBudget>(163840U);
 {
  graph::storage::detail::PropertyDecodeWorkspace workspace(budget);
  const auto result = store.readRangeFromManifest(snapshot, manifest, 0U, input.size());
  EXPECT_EQ(result, input);
  EXPECT_EQ(budget->used(), result.capacity());
 }
 EXPECT_EQ(budget->used(), 0U);
}
