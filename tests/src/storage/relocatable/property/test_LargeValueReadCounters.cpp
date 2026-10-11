#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

TEST_F(RelocatableMetadataStoreTest, LargeValueReadCountersCountOnlySuccessfullyReadOverlappingChunks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options;
	options.media_type = "application/octet-stream";
	options.first_data_slot = PhysicalSlotId(40U);
	options.last_data_slot = PhysicalSlotId(80U);
	options.first_manifest_slot = PhysicalSlotId(81U);
	options.last_manifest_slot = PhysicalSlotId(90U);
	std::vector<std::byte> payload(100000U, std::byte{0x37});
	const auto written = store.write(payload, options, LogSequenceNumber(2U));
	ASSERT_GT(written.manifest.chunks.size(), 1U);
	auto snapshot = metadata->openSnapshot();
	using graph::debug::PerfTrace;
	struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::reset(); } } cleanup;
	PerfTrace::setEnabled(false);
	PerfTrace::setValueCollectionEnabled(true);
	PerfTrace::reset();
	const auto offset = written.manifest.chunks[1U].uncompressed_offset;
	EXPECT_EQ(store.readRange(snapshot, written.reference, offset, 1U),
			  std::vector<std::byte>{std::byte{0x37}});
	const auto partial = PerfTrace::snapshotAndReset();
	ASSERT_TRUE(partial.contains("native_graph.read.large_value_chunks_read"));
	EXPECT_EQ(partial.at("native_graph.read.large_value_chunks_read").totalValue, 1);
	EXPECT_TRUE(store.readRange(snapshot, written.reference, offset, 0U).empty());
	EXPECT_FALSE(PerfTrace::snapshotAndReset().contains("native_graph.read.large_value_chunks_read"));
	EXPECT_EQ(store.read(snapshot, written.reference), payload);
	const auto full = PerfTrace::snapshotAndReset();
	ASSERT_TRUE(full.contains("native_graph.read.large_value_chunks_read"));
	EXPECT_EQ(full.at("native_graph.read.large_value_chunks_read").totalValue,
			  static_cast<int64_t>(written.manifest.chunks.size()));
}
