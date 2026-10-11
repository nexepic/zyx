/**
 * @file test_MetadataWriteBatch.cpp
 * @brief Validation, ownership, and single-publication tests for metadata COW batches.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	class ScriptedPageAllocator final : public MetadataPageAllocator {
	public:
		std::deque<MetadataPageId> pageIds;
		std::deque<PhysicalPageRef> pageReferences;
		std::deque<std::optional<PhysicalSlotId>> createdPoolSlots;
		std::vector<PhysicalPageRef> abandoned;
		std::vector<PhysicalSlotId> abandonedPools;
		std::vector<Superblock> begunBatches;
		std::vector<Superblock> endedBatches;
		size_t metadataReservationCalls = 0U;
		size_t metadataReservationReleases = 0U;
		uint64_t nextPageId = 1;
		uint64_t nextSlot = 1;

		MetadataPageId allocateMetadataPageId() override {
			if (!pageIds.empty()) {
				const MetadataPageId pageId = pageIds.front();
				pageIds.pop_front();
				return pageId;
			}
			return MetadataPageId(nextPageId++);
		}

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
											std::span<const PhysicalSlotId>) override {
			const std::optional<PhysicalSlotId> createdPool = [&]() {
				if (createdPoolSlots.empty()) {
					return std::optional<PhysicalSlotId>{};
				}
				const std::optional<PhysicalSlotId> result = createdPoolSlots.front();
				createdPoolSlots.pop_front();
				return result;
			}();
			if (!pageReferences.empty()) {
				const PhysicalPageRef reference = pageReferences.front();
				pageReferences.pop_front();
				return {reference, createdPool};
			}
			return {PhysicalPageRef{PhysicalSlotId(nextSlot++), 1, generation}, createdPool};
		}

		void abandonPage(PhysicalPageRef reference) noexcept override { abandoned.push_back(reference); }
		void abandonUnpublishedPool(PhysicalSlotId slot) noexcept override { abandonedPools.push_back(slot); }
		void beginUnpublishedBatch(const Superblock &current) override { begunBatches.push_back(current); }
		void endUnpublishedBatch(const Superblock &current) noexcept override { endedBatches.push_back(current); }
		void reserveMetadataFrames(size_t, std::span<const PhysicalSlotId>, std::span<const PhysicalSlotRange>,
								   const MetadataSlotReservationLookup *) override {
			++metadataReservationCalls;
		}
		void releaseMetadataFrames() noexcept override { ++metadataReservationReleases; }
	};

} // namespace

class RelocatableMetadataWriteBatchTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath =
				std::filesystem::temp_directory_path() / ("relocatable_metadata_batch_" + to_string(uuid) + ".dat");
		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(testFilePath, error);
	}
};

TEST_F(RelocatableMetadataWriteBatchTest, ValidatesConstructionAllocatorResultsAndRoots) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	expectFormatError([&] { static_cast<void>(MetadataWriteBatch(allocator, initial, LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(MetadataWriteBatch(allocator, initial, LogSequenceNumber(1))); },
					  FormatError::FERR_INVALID_STATE);
	const Superblock maximumGeneration = superblock(std::numeric_limits<uint64_t>::max() >> 5U, 1);
	expectFormatError(
			[&] { static_cast<void>(MetadataWriteBatch(allocator, maximumGeneration, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	allocator.pageIds.push_back(MetadataPageId{});
	expectFormatError([&] { static_cast<void>(batch.allocateMetadataPageId()); }, FormatError::FERR_INVALID_STATE);
	allocator.pageReferences.push_back(PhysicalPageRef{});
	expectFormatError([&] { static_cast<void>(batch.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	allocator.pageReferences.push_back(PhysicalPageRef{PhysicalSlotId(1), 1, Generation(1)});
	expectFormatError([&] { static_cast<void>(batch.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setCatalogRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setAllocatorRoot(PhysicalPageRef{PhysicalSlotId(1), 1, Generation(1)}); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setCatalogRoot(PhysicalPageRef{PhysicalSlotId(99), 1, Generation(2)}); },
					  FormatError::FERR_INVALID_STATE);
	const PhysicalPageRef owned = batch.allocatePage();
	expectFormatError(
			[&] { batch.setCatalogRoot(PhysicalPageRef{owned.physical_slot, owned.frame_index, Generation(1)}); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setAllocatorRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setAllocatorRoot(PhysicalPageRef{PhysicalSlotId(99), 1, Generation(2)}); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setPageDirectoryRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setExtentDirectoryRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setNodeEntityDirectoryRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setEdgeEntityDirectoryRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setEntityPropertyDirectoryRoot(PhysicalPageRef{}); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setIndexRootDirectoryRoot(PhysicalPageRef{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setLargeValueManifestDirectoryRoot(PhysicalPageRef{}); },
					  FormatError::FERR_INVALID_STATE);
	const PhysicalPageRef directoryRoot = batch.allocatePage();
	expectFormatError(
			[&] {
				batch.setPageDirectoryRoot(
						PhysicalPageRef{directoryRoot.physical_slot, directoryRoot.frame_index, Generation(1)});
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setExtentDirectoryRoot(PhysicalPageRef{PhysicalSlotId(99), 1, Generation(2)}); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				batch.setNodeEntityDirectoryRoot(
						PhysicalPageRef{directoryRoot.physical_slot, directoryRoot.frame_index, Generation(1)});
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.setEdgeEntityDirectoryRoot(PhysicalPageRef{PhysicalSlotId(99), 1, Generation(2)}); },
					  FormatError::FERR_INVALID_STATE);
	batch.setPageDirectoryRoot(directoryRoot);
	batch.setExtentDirectoryRoot(directoryRoot);
	batch.setNodeEntityDirectoryRoot(directoryRoot);
	batch.setEdgeEntityDirectoryRoot(directoryRoot);
	batch.setEntityPropertyDirectoryRoot(directoryRoot);
	batch.setIndexRootDirectoryRoot(directoryRoot);
	batch.setLargeValueManifestDirectoryRoot(directoryRoot);
	EXPECT_EQ(batch.candidate().node_entity_directory_root, directoryRoot);
	EXPECT_EQ(batch.candidate().edge_entity_directory_root, directoryRoot);
	EXPECT_EQ(batch.candidate().entity_property_directory_root, directoryRoot);
	EXPECT_EQ(batch.candidate().index_root_directory_root, directoryRoot);
	EXPECT_EQ(batch.candidate().large_value_manifest_directory_root, directoryRoot);
	const PhysicalPageRef bootstrapRoot = batch.allocatePage();
	batch.setPageDirectoryBootstrapRoot(bootstrapRoot);
	EXPECT_TRUE(batch.candidate().usesPageDirectoryBootstrap());
	expectFormatError([&] { batch.setPageDirectoryRoot(batch.allocatePage()); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataWriteBatchTest, AllocatesAndObservesLogicalPageIdsWithoutReuse) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalPageId(), PageId(1));
	EXPECT_EQ(batch.allocateLogicalPageId(), PageId(2));
	batch.observeLogicalPageId(PageId(1));
	EXPECT_EQ(batch.candidate().next_logical_page_id, 3U);
	batch.observeLogicalPageId(PageId(9));
	EXPECT_EQ(batch.candidate().next_logical_page_id, 10U);
	expectFormatError([&] { batch.observeLogicalPageId(PageId{}); }, FormatError::FERR_INVALID_HEADER);

	Superblock maximum = initial;
	maximum.next_logical_page_id = std::numeric_limits<uint64_t>::max();
	MetadataWriteBatch finalId(allocator, maximum, LogSequenceNumber(2));
	EXPECT_EQ(finalId.allocateLogicalPageId(), PageId(std::numeric_limits<uint64_t>::max()));
	EXPECT_EQ(finalId.candidate().next_logical_page_id, 0U);
	expectFormatError([&] { static_cast<void>(finalId.allocateLogicalPageId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	Superblock exhausted = initial;
	exhausted.next_logical_page_id = 0U;
	MetadataWriteBatch exhaustedBatch(allocator, exhausted, LogSequenceNumber(2));
	exhaustedBatch.observeLogicalPageId(PageId(1));
	EXPECT_EQ(exhaustedBatch.candidate().next_logical_page_id, 0U);
	expectFormatError([&] { static_cast<void>(exhaustedBatch.allocateLogicalPageId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, RecordsFinalMetadataPageRouteFromOwnedCurrentFrame) {
	ScriptedPageAllocator allocator;
	MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
	const PhysicalPageRef reference = batch.allocatePage();
	batch.registerMetadataPageRoute(MetadataPageId(8U), reference);
	EXPECT_EQ(batch.metadataPageRoutes(), (std::vector<MetadataPageRoute>{{MetadataPageId(8U), reference}}));

	const PhysicalPageRef replacement = batch.allocatePage();
	batch.registerMetadataPageRoute(MetadataPageId(8U), replacement);
	EXPECT_EQ(batch.metadataPageRoutes(), (std::vector<MetadataPageRoute>{{MetadataPageId(8U), replacement}}));
	expectFormatError(
			[&] { batch.registerMetadataPageRoute(MetadataPageId(9U), {PhysicalSlotId(99U), 1U, Generation(2U)}); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { batch.registerMetadataPageRoute(MetadataPageId{}, reference); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataWriteBatchTest, ReleasesMetadataFrameReservationWhenAbandoned) {
	ScriptedPageAllocator allocator;
	{
		MetadataWriteBatch batch(allocator, superblock(1U, 1U), LogSequenceNumber(2U));
		batch.reserveMetadataFrames(2U);
		EXPECT_EQ(allocator.metadataReservationCalls, 1U);
		EXPECT_EQ(allocator.metadataReservationReleases, 0U);
	}
	EXPECT_EQ(allocator.metadataReservationReleases, 1U);
}

TEST_F(RelocatableMetadataWriteBatchTest, ReservesContiguousLogicalPageIdRangesWithoutAIdentityVector) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalPageIdRange(3U), PageId(1U));
	EXPECT_EQ(batch.candidate().next_logical_page_id, 4U);
	EXPECT_EQ(batch.allocateLogicalPageId(), PageId(4U));
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalPageIdRange(0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	Superblock maximum = initial;
	maximum.next_logical_page_id = std::numeric_limits<uint64_t>::max() - 1U;
	MetadataWriteBatch finalRange(allocator, maximum, LogSequenceNumber(2));
	EXPECT_EQ(finalRange.allocateLogicalPageIdRange(2U), PageId(std::numeric_limits<uint64_t>::max() - 1U));
	EXPECT_EQ(finalRange.candidate().next_logical_page_id, 0U);
	expectFormatError([&] { static_cast<void>(finalRange.allocateLogicalPageIdRange(1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, AllocatesAndObservesLogicalSegmentIdsWithoutReuse) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalSegmentId(), SegmentId(1));
	EXPECT_EQ(batch.allocateLogicalSegmentId(), SegmentId(2));
	batch.observeLogicalSegmentId(SegmentId(1));
	EXPECT_EQ(batch.candidate().next_logical_segment_id, 3U);
	batch.observeLogicalSegmentId(SegmentId(9));
	EXPECT_EQ(batch.candidate().next_logical_segment_id, 10U);
	expectFormatError([&] { batch.observeLogicalSegmentId(SegmentId{}); }, FormatError::FERR_INVALID_HEADER);

	Superblock maximum = initial;
	maximum.next_logical_segment_id = std::numeric_limits<uint64_t>::max();
	MetadataWriteBatch finalId(allocator, maximum, LogSequenceNumber(2));
	EXPECT_EQ(finalId.allocateLogicalSegmentId(), SegmentId(std::numeric_limits<uint64_t>::max()));
	EXPECT_EQ(finalId.candidate().next_logical_segment_id, 0U);
	expectFormatError([&] { static_cast<void>(finalId.allocateLogicalSegmentId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	Superblock exhausted = initial;
	exhausted.next_logical_segment_id = 0U;
	MetadataWriteBatch exhaustedBatch(allocator, exhausted, LogSequenceNumber(2));
	exhaustedBatch.observeLogicalSegmentId(SegmentId(1));
	EXPECT_EQ(exhaustedBatch.candidate().next_logical_segment_id, 0U);
	expectFormatError([&] { static_cast<void>(exhaustedBatch.allocateLogicalSegmentId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, AllocatesAndObservesLogicalEntityIdsWithoutReuse) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalNodeId(), 1U);
	EXPECT_EQ(batch.allocateLogicalEdgeId(), 1U);
	EXPECT_EQ(batch.allocateLogicalNodeId(), 2U);
	EXPECT_EQ(batch.allocateLogicalEdgeId(), 2U);
	batch.observeLogicalNodeId(9U);
	batch.observeLogicalEdgeId(19U);
	EXPECT_EQ(batch.candidate().next_logical_node_id, 10U);
	EXPECT_EQ(batch.candidate().next_logical_edge_id, 20U);
	expectFormatError([&] { batch.observeLogicalNodeId(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { batch.observeLogicalEdgeId(0U); }, FormatError::FERR_INVALID_HEADER);

	Superblock maximum = initial;
	maximum.next_logical_node_id = std::numeric_limits<uint64_t>::max();
	maximum.next_logical_edge_id = std::numeric_limits<uint64_t>::max();
	MetadataWriteBatch finalIds(allocator, maximum, LogSequenceNumber(2));
	EXPECT_EQ(finalIds.allocateLogicalNodeId(), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.allocateLogicalEdgeId(), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.candidate().next_logical_node_id, 0U);
	EXPECT_EQ(finalIds.candidate().next_logical_edge_id, 0U);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLogicalNodeId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLogicalEdgeId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, ReservesLogicalEntityIdRangesInConstantMetadataWork) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalNodeIdRange(3U), 1U);
	EXPECT_EQ(batch.allocateLogicalEdgeIdRange(5U), 1U);
	EXPECT_EQ(batch.candidate().next_logical_node_id, 4U);
	EXPECT_EQ(batch.candidate().next_logical_edge_id, 6U);

	Superblock final = initial;
	final.next_logical_node_id = std::numeric_limits<uint64_t>::max();
	final.next_logical_edge_id = std::numeric_limits<uint64_t>::max();
	MetadataWriteBatch finalIds(allocator, final, LogSequenceNumber(2));
	EXPECT_EQ(finalIds.allocateLogicalNodeIdRange(1U), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.allocateLogicalEdgeIdRange(1U), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.candidate().next_logical_node_id, 0U);
	EXPECT_EQ(finalIds.candidate().next_logical_edge_id, 0U);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLogicalNodeIdRange(1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLogicalEdgeIdRange(1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalNodeIdRange(0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalEdgeIdRange(0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, AllocatesAndObservesDerivedValueAndTokenIdsWithoutReuse) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	EXPECT_EQ(batch.allocateLogicalVectorId(), 1U);
	EXPECT_EQ(batch.allocateLargeValueId(), LargeValueId(1U));
	EXPECT_EQ(batch.allocateLargeValueIdRange(2U), LargeValueId(2U));
	EXPECT_EQ(batch.allocatePropertyKeyToken(), 1U);
	EXPECT_EQ(batch.allocateSchemaTokenId(), 1U);
	// Re-observing already issued identities must never regress the durable
	// high-water marks before the batch observes newer values.
	batch.observeLogicalVectorId(1U);
	batch.observeLargeValueId(LargeValueId(1U));
	batch.observePropertyKeyToken(1U);
	batch.observeSchemaTokenId(1U);
	EXPECT_EQ(batch.candidate().next_logical_vector_id, 2U);
	EXPECT_EQ(batch.candidate().next_large_value_id, 4U);
	EXPECT_EQ(batch.candidate().next_property_key_token, 2U);
	EXPECT_EQ(batch.candidate().next_schema_token_id, 2U);
	batch.observeLogicalVectorId(14U);
	batch.observeLargeValueId(LargeValueId(24U));
	batch.observePropertyKeyToken(19U);
	batch.observeSchemaTokenId(29U);
	EXPECT_EQ(batch.candidate().next_logical_vector_id, 15U);
	EXPECT_EQ(batch.candidate().next_large_value_id, 25U);
	EXPECT_EQ(batch.candidate().next_property_key_token, 20U);
	EXPECT_EQ(batch.candidate().next_schema_token_id, 30U);
	expectFormatError([&] { batch.observeLogicalVectorId(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { batch.observeLargeValueId(LargeValueId{}); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { batch.observePropertyKeyToken(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { batch.observeSchemaTokenId(0U); }, FormatError::FERR_INVALID_HEADER);

	Superblock maximum = initial;
	maximum.next_logical_vector_id = std::numeric_limits<uint64_t>::max();
	maximum.next_large_value_id = std::numeric_limits<uint64_t>::max();
	maximum.next_property_key_token = std::numeric_limits<uint32_t>::max();
	maximum.next_schema_token_id = std::numeric_limits<uint64_t>::max();
	MetadataWriteBatch finalIds(allocator, maximum, LogSequenceNumber(2));
	EXPECT_EQ(finalIds.allocateLogicalVectorId(), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.allocateLargeValueId(), LargeValueId(std::numeric_limits<uint64_t>::max()));
	EXPECT_EQ(finalIds.allocatePropertyKeyToken(), std::numeric_limits<uint32_t>::max());
	EXPECT_EQ(finalIds.allocateSchemaTokenId(), std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(finalIds.candidate().next_logical_vector_id, 0U);
	EXPECT_EQ(finalIds.candidate().next_large_value_id, 0U);
	EXPECT_EQ(finalIds.candidate().next_property_key_token, 0U);
	EXPECT_EQ(finalIds.candidate().next_schema_token_id, 0U);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLogicalVectorId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(finalIds.allocateLargeValueId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(finalIds.allocatePropertyKeyToken()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(finalIds.allocateSchemaTokenId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);

	Superblock rangeMaximum = initial;
	rangeMaximum.next_large_value_id = std::numeric_limits<uint64_t>::max() - 1U;
	MetadataWriteBatch finalRange(allocator, rangeMaximum, LogSequenceNumber(2));
	EXPECT_EQ(finalRange.allocateLargeValueIdRange(2U), LargeValueId(std::numeric_limits<uint64_t>::max() - 1U));
	EXPECT_EQ(finalRange.candidate().next_large_value_id, 0U);
	expectFormatError([&] { static_cast<void>(finalRange.allocateLargeValueIdRange(1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLargeValueIdRange(0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataWriteBatchTest, RejectsDuplicateLocationsAndReturnsUnpublishedPages) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	const PhysicalPageRef reference{PhysicalSlotId(1), 1, Generation(2)};
	allocator.pageReferences = {reference, reference};
	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		EXPECT_EQ(batch.allocatePage(), reference);
		expectFormatError([&] { static_cast<void>(batch.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	}
	ASSERT_EQ(allocator.abandoned.size(), 1U);
	EXPECT_EQ(allocator.abandoned.front(), reference);
}

TEST_F(RelocatableMetadataWriteBatchTest, RejectsDuplicateLocationsAfterManyDistinctAllocatedFrames) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	constexpr uint64_t pageCount = 4096U;
	for (uint64_t slot = 1U; slot <= pageCount; ++slot) {
		allocator.pageReferences.push_back(PhysicalPageRef{PhysicalSlotId(slot), 1U, Generation(2U)});
	}
	allocator.pageReferences.push_back(PhysicalPageRef{PhysicalSlotId(pageCount), 1U, Generation(2U)});

	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		for (uint64_t slot = 1U; slot <= pageCount; ++slot) {
			EXPECT_EQ(batch.allocatePage(), PhysicalPageRef(PhysicalSlotId(slot), 1U, Generation(2U)));
		}
		expectFormatError([&] { static_cast<void>(batch.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	}

	EXPECT_EQ(allocator.abandoned.size(), pageCount);
}

TEST_F(RelocatableMetadataWriteBatchTest, ReturnsNewMetadataPoolWhenItsCandidateIsNotPublished) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	const PhysicalPageRef reference{PhysicalSlotId(7), 1, Generation(2)};
	allocator.pageReferences = {reference};
	allocator.createdPoolSlots = {PhysicalSlotId(7)};
	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		EXPECT_EQ(batch.allocatePage(), reference);
		ASSERT_EQ(batch.createdPoolCount(), 1U);
		EXPECT_EQ(batch.createdPoolSlot(0), PhysicalSlotId(7));
		expectFormatError([&] { static_cast<void>(batch.createdPoolSlot(1)); }, FormatError::FERR_INVALID_STATE);
	}
	ASSERT_EQ(allocator.abandoned.size(), 1U);
	EXPECT_EQ(allocator.abandoned.front(), reference);
	ASSERT_EQ(allocator.abandonedPools.size(), 1U);
	EXPECT_EQ(allocator.abandonedPools.front(), PhysicalSlotId(7));
}

TEST_F(RelocatableMetadataWriteBatchTest, RejectsInvalidOrDuplicateDataAndPoolOwnershipRecords) {
	const Superblock initial = superblock(1, 1);

	ScriptedPageAllocator invalidDataAllocator;
	MetadataWriteBatch invalidData(invalidDataAllocator, initial, LogSequenceNumber(2));
	expectFormatError([&] { invalidData.reserveDataSlot(PhysicalSlotId{}); }, FormatError::FERR_INVALID_STATE);
	invalidData.reserveDataSlot(PhysicalSlotId(9));
	invalidData.reserveDataSlot(PhysicalSlotId(9));
	EXPECT_EQ(invalidData.allocatePage().physical_slot, PhysicalSlotId(1));

	ScriptedPageAllocator mismatchedPoolAllocator;
	mismatchedPoolAllocator.pageReferences = {PhysicalPageRef{PhysicalSlotId(1), 1, Generation(2)}};
	mismatchedPoolAllocator.createdPoolSlots = {PhysicalSlotId(2)};
	{
		MetadataWriteBatch mismatchedPool(mismatchedPoolAllocator, initial, LogSequenceNumber(2));
		expectFormatError([&] { static_cast<void>(mismatchedPool.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	}
	ASSERT_EQ(mismatchedPoolAllocator.abandoned.size(), 1U);
	EXPECT_EQ(mismatchedPoolAllocator.abandoned.front().physical_slot, PhysicalSlotId(1));

	ScriptedPageAllocator invalidPoolAllocator;
	invalidPoolAllocator.createdPoolSlots = {PhysicalSlotId{}};
	{
		MetadataWriteBatch invalidPool(invalidPoolAllocator, initial, LogSequenceNumber(2));
		expectFormatError([&] { static_cast<void>(invalidPool.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	}
	ASSERT_EQ(invalidPoolAllocator.abandoned.size(), 1U);
	EXPECT_EQ(invalidPoolAllocator.abandoned.front().physical_slot, PhysicalSlotId(1));

	ScriptedPageAllocator duplicatePoolAllocator;
	duplicatePoolAllocator.pageReferences = {PhysicalPageRef{PhysicalSlotId(3), 1, Generation(2)},
											 PhysicalPageRef{PhysicalSlotId(3), 2, Generation(2)}};
	duplicatePoolAllocator.createdPoolSlots = {PhysicalSlotId(3), PhysicalSlotId(3)};
	{
		MetadataWriteBatch duplicatePool(duplicatePoolAllocator, initial, LogSequenceNumber(2));
		static_cast<void>(duplicatePool.allocatePage());
		expectFormatError([&] { static_cast<void>(duplicatePool.allocatePage()); }, FormatError::FERR_INVALID_STATE);
	}
	EXPECT_EQ(duplicatePoolAllocator.abandoned.size(), 2U);
}

TEST_F(RelocatableMetadataWriteBatchTest, PublishesOnlyOnceAndRejectsStaleBootstrap) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	ScriptedPageAllocator allocator;
	MetadataWriteBatch published(allocator, initial, LogSequenceNumber(2));
	const PhysicalPageRef catalogRoot = published.allocatePage();
	const PhysicalPageRef freeSpaceRoot = published.allocatePage();
	published.setCatalogRoot(freeSpaceRoot);
	published.setAllocatorRoot(catalogRoot);
	published.publish(bootstrap);
	EXPECT_EQ(bootstrap.recover().catalog_root, freeSpaceRoot);
	EXPECT_EQ(bootstrap.recover().allocator_root, catalogRoot);
	expectFormatError([&] { published.publish(bootstrap); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.abandoned.empty());

	ScriptedPageAllocator staleAllocator;
	MetadataWriteBatch stale(staleAllocator, initial, LogSequenceNumber(3));
	const PhysicalPageRef staleRoot = stale.allocatePage();
	stale.setCatalogRoot(staleRoot);
	expectFormatError([&] { stale.publish(bootstrap); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataWriteBatchTest, AbandonsPagesWhenPublicationStopsBeforeInactiveSuperblockWrite) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);

	ScriptedPageAllocator allocator;
	PhysicalPageRef root;
	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		root = batch.allocatePage();
		batch.setCatalogRoot(root);
		EXPECT_THROW(batch.publish(bootstrap, BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE),
					 BootstrapPublishInterrupted);
	}
	EXPECT_EQ(bootstrap.recover(), initial);
	ASSERT_EQ(allocator.abandoned.size(), 1U);
	EXPECT_EQ(allocator.abandoned.front(), root);
}

TEST_F(RelocatableMetadataWriteBatchTest, AbandonsPagesAfterTornInactiveSuperblockWrite) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);

	ScriptedPageAllocator allocator;
	PhysicalPageRef root;
	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		root = batch.allocatePage();
		batch.setCatalogRoot(root);
		EXPECT_THROW(batch.publish(bootstrap, BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE),
					 BootstrapPublishInterrupted);
	}
	EXPECT_EQ(bootstrap.recover(), initial);
	ASSERT_EQ(allocator.abandoned.size(), 1U);
	EXPECT_EQ(allocator.abandoned.front(), root);
}

TEST_F(RelocatableMetadataWriteBatchTest, RetainsPagesAfterInactiveSuperblockIsDurable) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);

	ScriptedPageAllocator allocator;
	Superblock candidate;
	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
		const PhysicalPageRef root = batch.allocatePage();
		batch.setCatalogRoot(root);
		candidate = batch.candidate();
		EXPECT_THROW(batch.publish(bootstrap, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE),
					 BootstrapPublishInterrupted);
	}
	EXPECT_EQ(bootstrap.recover(), candidate);
	EXPECT_TRUE(allocator.abandoned.empty());
}

TEST_F(RelocatableMetadataWriteBatchTest, EndsTheAllocatorBatchContextAtPublicationOrAbandonment) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);

	ScriptedPageAllocator publishedAllocator;
	{
		MetadataWriteBatch batch(publishedAllocator, initial, LogSequenceNumber(2));
		EXPECT_EQ(publishedAllocator.begunBatches, std::vector{initial});
		EXPECT_TRUE(publishedAllocator.endedBatches.empty());
		batch.setCatalogRoot(batch.allocatePage());
		batch.publish(bootstrap);
		EXPECT_EQ(publishedAllocator.endedBatches, std::vector{initial});
	}
	EXPECT_EQ(publishedAllocator.endedBatches, std::vector{initial});

	ScriptedPageAllocator abandonedAllocator;
	{
		MetadataWriteBatch batch(abandonedAllocator, bootstrap.recover(), LogSequenceNumber(3));
		batch.setCatalogRoot(batch.allocatePage());
		EXPECT_THROW(batch.publish(bootstrap, BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE),
					 BootstrapPublishInterrupted);
		EXPECT_TRUE(abandonedAllocator.endedBatches.empty());
	}
	EXPECT_EQ(abandonedAllocator.begunBatches, std::vector{bootstrap.recover()});
	EXPECT_EQ(abandonedAllocator.endedBatches, std::vector{bootstrap.recover()});
}

TEST_F(RelocatableMetadataWriteBatchTest, ObservingMaximumLogicalIdentifiersPermanentlyExhaustsTheirNamespaces) {
	ScriptedPageAllocator allocator;
	const Superblock initial = superblock(1, 1);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	batch.observeLogicalPageId(PageId(std::numeric_limits<uint64_t>::max()));
	batch.observeLogicalSegmentId(SegmentId(std::numeric_limits<uint64_t>::max()));
	batch.observeLogicalNodeId(std::numeric_limits<uint64_t>::max());
	batch.observeLogicalEdgeId(std::numeric_limits<uint64_t>::max());
	batch.observeLogicalVectorId(std::numeric_limits<uint64_t>::max());
	batch.observeLargeValueId(LargeValueId(std::numeric_limits<uint64_t>::max()));
	batch.observePropertyKeyToken(std::numeric_limits<uint32_t>::max());
	batch.observeSchemaTokenId(std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(batch.candidate().next_logical_page_id, 0U);
	EXPECT_EQ(batch.candidate().next_logical_segment_id, 0U);
	EXPECT_EQ(batch.candidate().next_logical_node_id, 0U);
	EXPECT_EQ(batch.candidate().next_logical_edge_id, 0U);
	EXPECT_EQ(batch.candidate().next_logical_vector_id, 0U);
	EXPECT_EQ(batch.candidate().next_large_value_id, 0U);
	EXPECT_EQ(batch.candidate().next_property_key_token, 0U);
	EXPECT_EQ(batch.candidate().next_schema_token_id, 0U);

	expectFormatError([&] { static_cast<void>(batch.allocateLogicalPageId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalSegmentId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalNodeId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalEdgeId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLogicalVectorId()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateLargeValueId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocatePropertyKeyToken()); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(batch.allocateSchemaTokenId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
}
