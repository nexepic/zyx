/**
 * @file test_MetadataPagePool.cpp
 * @brief Codec, allocation, recovery, and COW integration tests for metadata page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

namespace {

	constexpr size_t HEADER_CRC_OFFSET = 40;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	void recalculateCrc(std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, graph::utils::calculateCrc(bytes.data(), bytes.size()));
	}

	void writeControlHeaderCopies(StorageIO &io, uint64_t poolOffset, const MetadataPagePoolHeader &header) {
		const auto bytes = MetadataPagePoolHeaderCodec::encode(header);
		io.writeAt(poolOffset + RSS_ENVELOPE_BYTES, bytes.data(), bytes.size());
		io.writeAt(poolOffset + RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES, bytes.data(), bytes.size());
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	CatalogEntry entry(uint64_t segmentId, uint64_t physicalSlot, uint64_t generation, uint64_t lsn) {
		CatalogEntry result;
		result.segment_id = SegmentId(segmentId);
		result.physical_slot = PhysicalSlotId(physicalSlot);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 19;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	struct CatalogTreePages {
		Superblock superblock;
		PhysicalPageRef root;
		PhysicalPageRef leaf;
	};

	CatalogTreePages writeSingleLeafTree(const Superblock &base, MetadataPageFile &pages,
										 MetadataPagePoolAllocator &allocator, SegmentId segmentId,
										 Generation rootGeneration, LogSequenceNumber rootLsn,
										 Generation leafGeneration, LogSequenceNumber leafLsn,
										 uint32_t rootChildIndex) {
		const PhysicalPageRef leafReference = allocator.allocatePage(leafGeneration);
		CatalogLeafPage leaf;
		leaf.page_id = allocator.allocateMetadataPageId();
		leaf.minimum_segment_id = segmentId;
		leaf.maximum_segment_id = segmentId;
		leaf.generation = leafGeneration;
		leaf.committed_lsn = leafLsn;
		leaf.entries = {entry(segmentId.value(), 100, leafGeneration.value(), leafLsn.value())};
		pages.writeCatalogLeaf(leafReference, leaf);

		const PhysicalPageRef rootReference = allocator.allocatePage(rootGeneration);
		CatalogRadixPage root;
		root.kind = CatalogRadixPageKind::CRPK_ROOT;
		root.level = 0;
		root.page_id = allocator.allocateMetadataPageId();
		root.generation = rootGeneration;
		root.committed_lsn = rootLsn;
		root.children[rootChildIndex] = leafReference;
		pages.writeCatalogRadix(rootReference, root);

		CatalogTreePages result;
		result.superblock = base;
		result.superblock.catalog_root = rootReference;
		result.root = rootReference;
		result.leaf = leafReference;
		return result;
	}

	CatalogRadixPage radixPage(MetadataPageId pageId, CatalogRadixPageKind kind, uint16_t level, Generation generation,
							   LogSequenceNumber lsn) {
		CatalogRadixPage page;
		page.kind = kind;
		page.level = level;
		page.page_id = pageId;
		page.generation = generation;
		page.committed_lsn = lsn;
		return page;
	}

	void assignLocationDirectoryRoot(Superblock &superblock, MetadataDirectoryType directoryType,
									 PhysicalPageRef root) {
		switch (directoryType) {
			case MetadataDirectoryType::MDT_PAGE:
				superblock.page_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_EXTENT:
				superblock.extent_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_NODE_ENTITY:
				superblock.node_entity_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_EDGE_ENTITY:
				superblock.edge_entity_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_ENTITY_PROPERTY:
				superblock.entity_property_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_INDEX_ROOT:
				superblock.index_root_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_VECTOR_LOCATION:
				superblock.vector_location_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_VECTOR_MANIFEST:
				superblock.vector_manifest_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST:
				superblock.large_value_manifest_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_STATE:
				superblock.state_directory_root = root;
				return;
			case MetadataDirectoryType::MDT_RETIRE_QUEUE:
				superblock.retire_queue_root = root;
				return;
			default:
				throw std::invalid_argument("test helper requires a Location Directory type");
		}
	}

	PhysicalPageRef writeLocationDirectoryRoot(MetadataPageFile &pages, MetadataPagePoolAllocator &allocator,
											   MetadataDirectoryType directoryType, Superblock &superblock) {
		const PhysicalPageRef leafReference = allocator.allocatePage(Generation(1));
		LocationLeafPage leaf;
		leaf.directory_type = directoryType;
		leaf.page_id = allocator.allocateMetadataPageId();
		leaf.generation = Generation(1);
		leaf.committed_lsn = LogSequenceNumber(1);
		leaf.entries = {{1U, SegmentId(1), Generation(1), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, 0U}};
		pages.writeLocationLeaf(leafReference, leaf);

		const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
		MetadataRadixPage root;
		root.kind = CatalogRadixPageKind::CRPK_ROOT;
		root.directory_type = directoryType;
		root.level = 0;
		root.page_id = allocator.allocateMetadataPageId();
		root.generation = Generation(1);
		root.committed_lsn = LogSequenceNumber(1);
		root.children[0] = leafReference;
		pages.writeMetadataRadix(rootReference, root);
		assignLocationDirectoryRoot(superblock, directoryType, rootReference);
		return rootReference;
	}

} // namespace

TEST(RelocatableMetadataPagePoolCodecTest, MatchesGoldenFieldsRoundTripsAndRejectsMalformedHeaders) {
	MetadataPagePoolHeader header;
	header.physical_slot = PhysicalSlotId(7);
	header.segment_id = SegmentId(19);
	header.allocated_frames = (1U << 0U) | (1U << 30U);
	header.generation = Generation(3);
	header.committed_lsn = LogSequenceNumber(9);
	header.control_sequence = 42U;
	const auto encoded = MetadataPagePoolHeaderCodec::encode(header);
	EXPECT_EQ(encoded.size(), RSS_METADATA_PAGE_POOL_HEADER_BYTES);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x50U);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 20), 2U);
	EXPECT_EQ(MetadataPagePoolHeaderCodec::decode(encoded).allocated_frames, header.allocated_frames);
	EXPECT_EQ(MetadataPagePoolHeaderCodec::decode(encoded).control_sequence, header.control_sequence);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(std::span(encoded).first(63))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{0});
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_METADATA_PAGE_POOL_VERSION + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6, RSS_METADATA_PAGE_POOL_HEADER_BYTES - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 20, 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 16, 0x80000000U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, 48, 0U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataPagePoolHeader invalid = header;
	invalid.physical_slot = PhysicalSlotId{};
	expectFormatError([&] { static_cast<void>(MetadataPagePoolHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableMetadataPagePoolCodecTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	MetadataPagePoolHeader header;
	header.physical_slot = PhysicalSlotId(7U);
	header.segment_id = SegmentId(19U);
	header.allocated_frames = (1U << 0U) | (1U << 30U);
	header.generation = Generation(3U);
	header.committed_lsn = LogSequenceNumber(9U);
	header.control_sequence = 42U;
	const auto encoded = MetadataPagePoolHeaderCodec::encode(header);
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateCrc(mutation);
			try {
				const MetadataPagePoolHeader decoded = MetadataPagePoolHeaderCodec::decode(mutation);
				if (MetadataPagePoolHeaderCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "metadata pool-header mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}
}

TEST_F(RelocatableMetadataPagePoolTest, AllocatesFramesAcrossPoolsAndReusesOnlyAbandonedPages) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	EXPECT_EQ(allocator.allocateMetadataPageId(), MetadataPageId(1));
	const PhysicalPageRef first = allocator.allocatePage(Generation(2));
	EXPECT_EQ(first.physical_slot, PhysicalSlotId(1));
	EXPECT_EQ(first.frame_index, 1U);
	for (uint32_t frame = 2; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		const PhysicalPageRef allocated = allocator.allocatePage(Generation(2));
		EXPECT_EQ(allocated.physical_slot, PhysicalSlotId(1));
		EXPECT_EQ(allocated.frame_index, frame);
	}
	const PhysicalPageRef secondPool = allocator.allocatePage(Generation(2));
	EXPECT_EQ(secondPool.physical_slot, PhysicalSlotId(2));
	EXPECT_EQ(secondPool.frame_index, 1U);
	const MetadataPagePoolStats fullStats = allocator.stats();
	EXPECT_EQ(fullStats.pools, 2U);
	EXPECT_EQ(fullStats.allocated_pages, RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
	EXPECT_EQ(fullStats.free_pages, RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
	EXPECT_EQ(io->fileSize(), RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE);

	allocator.abandonPage(first);
	const PhysicalPageRef reused = allocator.allocatePage(Generation(3));
	EXPECT_EQ(reused.physical_slot, first.physical_slot);
	EXPECT_EQ(reused.frame_index, first.frame_index);
	EXPECT_EQ(reused.generation, Generation(3));
	EXPECT_EQ(allocator.allocateMetadataPageId(), MetadataPageId(2));
	allocator.abandonPage(reused);
	allocator.abandonPage(reused);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameReservationFailsBeforeChangingAnExhaustedAllocator) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	std::vector<PhysicalPageRef> allocated;
	allocated.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		allocated.push_back(allocator.allocatePage(Generation(1)));
	}
	const MetadataPagePoolStats before = allocator.stats();
	const uint64_t fileSize = io->fileSize();
	expectFormatError(
			[&] {
				allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{},
									   std::span<const PhysicalSlotRange>{}, nullptr);
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(allocator.stats().allocated_pages, before.allocated_pages);
	EXPECT_EQ(io->fileSize(), fileSize);

	allocator.abandonPage(allocated.back());
	allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{},
								   std::span<const PhysicalSlotRange>{}, nullptr);
	const PhysicalPageRef reused = allocator.allocatePage(Generation(2));
	EXPECT_EQ(reused.physical_slot, allocated.back().physical_slot);
	EXPECT_EQ(reused.frame_index, allocated.back().frame_index);
	allocator.releaseMetadataFrames();
}

TEST_F(RelocatableMetadataPagePoolTest, MetadataFrameReservationPreventsPoolGrowthUntilReleased) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	for (uint32_t frame = 0U; frame + 1U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		static_cast<void>(allocator.allocatePage(Generation(1)));
	}
	allocator.reserveMetadataFrames(1U, std::span<const PhysicalSlotId>{},
							   std::span<const PhysicalSlotRange>{}, nullptr);
	static_cast<void>(allocator.allocatePage(Generation(2)));
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(2))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(allocator.stats().pools, 1U);
	allocator.releaseMetadataFrames();
	const MetadataPageAllocation appended = allocator.allocatePage(
			Generation(2), LogSequenceNumber(2), std::span<const PhysicalSlotId>{});
	EXPECT_EQ(appended.created_pool_slot, std::optional<PhysicalSlotId>(PhysicalSlotId(2)));
}

TEST_F(RelocatableMetadataPagePoolTest, SkipsReservedDataTailSlotsWhenCreatingAMetadataPool) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const std::array reservedDataSlots{PhysicalSlotId(1)};

	const MetadataPageAllocation allocation =
			allocator.allocatePage(Generation(1), LogSequenceNumber(1), reservedDataSlots);
	EXPECT_EQ(allocation.page.physical_slot, PhysicalSlotId(2));
	ASSERT_TRUE(allocation.created_pool_slot.has_value());
	EXPECT_EQ(*allocation.created_pool_slot, PhysicalSlotId(2));
	EXPECT_EQ(allocator.physicalFileSize(), RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE);
	EXPECT_TRUE(allocator.isMetadataPoolSlot(PhysicalSlotId(2)));
	EXPECT_FALSE(allocator.isMetadataPoolSlot(PhysicalSlotId(1)));
}

TEST_F(RelocatableMetadataPagePoolTest, UsesBatchReservationLookupForExactSlotsAndRanges) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));

	batch.reserveDataSlot(PhysicalSlotId(1U));
	batch.reserveDataSlotRange({PhysicalSlotId(2U), PhysicalSlotId(1'000U)});
	const PhysicalPageRef allocation = batch.allocatePage();

	EXPECT_EQ(allocation.physical_slot, PhysicalSlotId(1'001U));
	ASSERT_EQ(batch.createdPoolCount(), 1U);
	EXPECT_EQ(batch.createdPoolSlot(0U), PhysicalSlotId(1'001U));
	EXPECT_TRUE(batch.isDataSlotReserved(PhysicalSlotId(1U)));
	EXPECT_TRUE(batch.isDataSlotReserved(PhysicalSlotId(500U)));
	EXPECT_FALSE(batch.isDataSlotReserved(PhysicalSlotId(1'001U)));
}

TEST_F(RelocatableMetadataPagePoolTest, ReclaimsInterleavedUnpublishedDataAndMetadataTailSlotsBeforeRootPublication) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	nativeIo->enableBufferPool(8U * RSS_PAGE_SIZE);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	const uint64_t initialFileSize = nativeIo->fileSize();

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		{
			MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
			batch.reserveDataSlot(PhysicalSlotId(1U));
			batch.reserveDataSlot(PhysicalSlotId(3U));
			batch.reserveDataSlot(PhysicalSlotId(6U));
			for (uint32_t index = 0U; index <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
				static_cast<void>(batch.allocatePage());
			}
			ASSERT_EQ(batch.createdPoolCount(), 2U);
			EXPECT_EQ(batch.createdPoolSlot(0U), PhysicalSlotId(2U));
			EXPECT_EQ(batch.createdPoolSlot(1U), PhysicalSlotId(4U));
			const std::byte zero{};
			const uint64_t reservedDataTail = initial.data_region_offset + 6U * RSS_SEGMENT_SIZE - 1U;
			nativeIo->writeAt(reservedDataTail, &zero, sizeof(zero));
			EXPECT_EQ(nativeIo->fileSize(), initialFileSize + 6U * RSS_SEGMENT_SIZE);
			EXPECT_THROW(batch.publish(bootstrap, BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE),
						 BootstrapPublishInterrupted);
		}
		EXPECT_EQ(nativeIo->fileSize(), initialFileSize);
		EXPECT_EQ(allocator.stats().pools, 0U);
		EXPECT_EQ(allocator.stats().allocated_pages, 0U);
	}
	nativeIo->disableBufferPool();
	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);

	auto reopened = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_NO_THROW(static_cast<void>(MetadataPagePoolAllocator(reopened, initial)));
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsUnknownFramesAndOutOfRangeAllocationGenerations) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const Generation outOfRangeGeneration((std::numeric_limits<uint64_t>::max() >> 5U) + 1U);
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(outOfRangeGeneration)); },
					  FormatError::FERR_INVALID_HEADER);

	static_cast<void>(allocator.allocatePage(Generation(1)));
	allocator.abandonPage(PhysicalPageRef{PhysicalSlotId(2), 1, Generation(1)});
	EXPECT_TRUE(allocator.stats().poisoned);
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(1))); },
					  FormatError::FERR_INVALID_STATE);

	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, ReopensPublishedTreesAndReclaimsUnpublishedFrames) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock published = tree.upsertAndPublish(
			bootstrap, initial, std::vector{entry(1, 100, 1, 2), entry(85, 101, 1, 2)}, LogSequenceNumber(2));
	EXPECT_EQ(allocator.stats().allocated_pages, 3U);
	allocator.recover(published);
	EXPECT_EQ(allocator.stats().allocated_pages, 3U);
	const PhysicalPageRef orphan = allocator.allocatePage(Generation(3));
	EXPECT_TRUE(orphan.isValid());
	EXPECT_EQ(allocator.stats().allocated_pages, 4U);
	io->sync();

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	CatalogTree reopenedTree(reopenedPages, reopenedAllocator);
	EXPECT_EQ(recovered.catalog_root, published.catalog_root);
	EXPECT_EQ(reopenedAllocator.stats().allocated_pages, 3U);
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(85)).physical_slot, PhysicalSlotId(101));
	EXPECT_EQ(reopenedAllocator.allocateMetadataPageId(), MetadataPageId(4));
	const PhysicalPageRef recycledOrphanLocation = reopenedAllocator.allocatePage(Generation(3));
	EXPECT_EQ(recycledOrphanLocation.physical_slot, orphan.physical_slot);
	EXPECT_EQ(recycledOrphanLocation.frame_index, orphan.frame_index);
}

TEST_F(RelocatableMetadataPagePoolTest, ReopensPublishedAllocatorDirectoryPagesWithoutReclaimingThem) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef leafReference = allocator.allocatePage(Generation(1));
	AllocatorLeafPage leaf;
	leaf.page_id = allocator.allocateMetadataPageId();
	leaf.first_slot = PhysicalSlotId(1);
	leaf.slot_count = 32;
	leaf.generation = Generation(1);
	leaf.committed_lsn = LogSequenceNumber(1);
	leaf.allocated_slots[0] = std::byte{0b00000011};
	pages.writeAllocatorLeaf(leafReference, leaf);

	const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
	MetadataRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0, Generation(1),
									   LogSequenceNumber(1));
	root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	root.children[0] = leafReference;
	root.allocator_free_children[0] = std::byte{0x01};
	pages.writeMetadataRadix(rootReference, root);

	Superblock published = initial;
	published.generation = Generation(2);
	published.checkpoint_lsn = LogSequenceNumber(2);
	published.allocator_root = rootReference;
	bootstrap.publish(published);
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	EXPECT_EQ(recovered.allocator_root, rootReference);
	EXPECT_EQ(reopenedAllocator.stats().allocated_pages, 2U);
}

TEST_F(RelocatableMetadataPagePoolTest, OnlineReclamationPreservesCatalogPagesReferencedByActiveSnapshots) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);

	const Superblock generationTwo =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 2, 2)}, LogSequenceNumber(2));
	const Superblock generationThree =
			tree.upsertAndPublish(bootstrap, generationTwo, std::vector{entry(1, 101, 3, 3)}, LogSequenceNumber(3));
	ASSERT_EQ(allocator.stats().allocated_pages, 4U);
	const MetadataPageId nextMetadataPageId = allocator.stats().next_page_id;

	allocator.reclaimUnreachable(generationThree, std::array{generationTwo});
	EXPECT_EQ(allocator.stats().allocated_pages, 4U);
	EXPECT_EQ(tree.find(generationTwo, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(generationThree, SegmentId(1)).physical_slot, PhysicalSlotId(101));
	EXPECT_EQ(allocator.stats().next_page_id, nextMetadataPageId);

	allocator.reclaimUnreachable(generationThree, {});
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);
	EXPECT_EQ(tree.find(generationThree, SegmentId(1)).physical_slot, PhysicalSlotId(101));
	EXPECT_EQ(allocator.stats().next_page_id, nextMetadataPageId);
	EXPECT_EQ(allocator.allocateMetadataPageId(), nextMetadataPageId);
}

TEST_F(RelocatableMetadataPagePoolTest, OnlineReclamationPreservesAllocatorPagesReferencedByActiveSnapshots) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	const Superblock generationTwo =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(40), true, LogSequenceNumber(2));
	const Superblock generationThree =
			directory.setAllocatedAndPublish(bootstrap, generationTwo, PhysicalSlotId(41), true, LogSequenceNumber(3));
	ASSERT_EQ(allocator.stats().allocated_pages, 4U);

	allocator.reclaimUnreachable(generationThree, std::array{generationTwo});
	EXPECT_EQ(allocator.stats().allocated_pages, 4U);
	EXPECT_TRUE(directory.isAllocated(generationTwo, PhysicalSlotId(40)));
	EXPECT_FALSE(directory.isAllocated(generationTwo, PhysicalSlotId(41)));
	EXPECT_TRUE(directory.isAllocated(generationThree, PhysicalSlotId(40)));
	EXPECT_TRUE(directory.isAllocated(generationThree, PhysicalSlotId(41)));

	allocator.reclaimUnreachable(generationThree, {});
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);
	EXPECT_TRUE(directory.isAllocated(generationThree, PhysicalSlotId(40)));
	EXPECT_TRUE(directory.isAllocated(generationThree, PhysicalSlotId(41)));
}

TEST_F(RelocatableMetadataPagePoolTest, OnlineReclamationRejectsInvisibleSnapshotsAndPoisonedTraversalRecovers) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock current =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 2, 2)}, LogSequenceNumber(2));

	Superblock futureGeneration = initial;
	futureGeneration.generation = Generation(3);
	expectFormatError([&] { allocator.reclaimUnreachable(current, std::array{futureGeneration}); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);

	Superblock differentDatabase = initial;
	differentDatabase.database_id = DatabaseId(initial.database_id.value() + 1U);
	expectFormatError([&] { allocator.reclaimUnreachable(current, std::array{differentDatabase}); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);

	Superblock futureLsn = initial;
	futureLsn.checkpoint_lsn = LogSequenceNumber(3);
	expectFormatError([&] { allocator.reclaimUnreachable(current, std::array{futureLsn}); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);

	Superblock differentDataRegion = initial;
	differentDataRegion.data_region_offset += RSS_SEGMENT_SIZE;
	expectFormatError([&] { allocator.reclaimUnreachable(current, std::array{differentDataRegion}); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);

	Superblock mismatchedCurrent = current;
	mismatchedCurrent.data_region_offset += RSS_SEGMENT_SIZE;
	expectFormatError([&] { allocator.reclaimUnreachable(mismatchedCurrent, {}); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(current);
	EXPECT_FALSE(allocator.stats().poisoned);

	Superblock unavailableRoot = current;
	unavailableRoot.catalog_root = PhysicalPageRef{PhysicalSlotId(99), 1, Generation(1)};
	expectFormatError([&] { allocator.reclaimUnreachable(unavailableRoot, {}); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(current);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsMalformedOrMisclassifiedDirectoryRoots) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef malformedLeafReference = allocator.allocatePage(Generation(1));
	AllocatorLeafPage malformedLeaf;
	malformedLeaf.page_id = allocator.allocateMetadataPageId();
	malformedLeaf.first_slot = PhysicalSlotId(2);
	malformedLeaf.slot_count = 8;
	malformedLeaf.generation = Generation(1);
	malformedLeaf.committed_lsn = LogSequenceNumber(1);
	pages.writeAllocatorLeaf(malformedLeafReference, malformedLeaf);
	const PhysicalPageRef malformedRootReference = allocator.allocatePage(Generation(1));
	MetadataRadixPage malformedRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0,
												Generation(1), LogSequenceNumber(1));
	malformedRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	malformedRoot.children[0] = malformedLeafReference;
	pages.writeMetadataRadix(malformedRootReference, malformedRoot);
	Superblock malformed = initial;
	malformed.allocator_root = malformedRootReference;
	expectFormatError([&] { allocator.recover(malformed); }, FormatError::FERR_INVALID_HEADER);
	allocator.recover(initial);

	const PhysicalPageRef wrongTypeReference = allocator.allocatePage(Generation(1));
	MetadataRadixPage wrongType = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0,
											Generation(1), LogSequenceNumber(1));
	pages.writeMetadataRadix(wrongTypeReference, wrongType);
	Superblock wrongFreeType = initial;
	wrongFreeType.allocator_root = wrongTypeReference;
	expectFormatError([&] { allocator.recover(wrongFreeType); }, FormatError::FERR_INVALID_HEADER);
	allocator.recover(initial);

	const PhysicalPageRef misclassifiedReference = allocator.allocatePage(Generation(1));
	MetadataRadixPage misclassified = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0,
												Generation(1), LogSequenceNumber(1));
	pages.writeMetadataRadix(misclassifiedReference, misclassified);
	Superblock misclassifiedDirectory = initial;
	misclassifiedDirectory.page_directory_root = misclassifiedReference;
	// Page directories are now supported; a Catalog-typed root is malformed rather than unsupported.
	expectFormatError([&] { allocator.recover(misclassifiedDirectory); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPagePoolTest, IgnoresWellFormedNonCatalogSlotsDuringPoolDiscovery) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(2)));
	io->sync();
	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	envelope.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size());
	io->sync();
	reopen();
	MetadataPagePoolAllocator reopenedAllocator(io, initial);
	EXPECT_EQ(reopenedAllocator.stats().pools, 0U);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsNonemptySlotsWithoutARelocatableEnvelopeAndReportsPoolMembership) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	EXPECT_FALSE(allocator.isMetadataPoolSlot(PhysicalSlotId{}));
	EXPECT_FALSE(allocator.isMetadataPoolSlot(PhysicalSlotId(1)));
	EXPECT_FALSE(allocator.isMetadataPoolEmpty(PhysicalSlotId{}));
	EXPECT_FALSE(allocator.isMetadataPoolEmpty(PhysicalSlotId(1)));

	const PhysicalPageRef page = allocator.allocatePage(Generation(1));
	EXPECT_TRUE(allocator.isMetadataPoolSlot(PhysicalSlotId(1)));
	EXPECT_FALSE(allocator.isMetadataPoolEmpty(PhysicalSlotId(1)));
	EXPECT_FALSE(allocator.isMetadataPoolSlot(PhysicalSlotId(2)));
	EXPECT_FALSE(allocator.isMetadataPoolEmpty(PhysicalSlotId(2)));
	expectFormatError([&] { static_cast<void>(allocator.poolHeader(PhysicalSlotId{})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(allocator.poolHeader(PhysicalSlotId(2U))); },
					  FormatError::FERR_INVALID_STATE);
	allocator.abandonPage(page);
	EXPECT_TRUE(allocator.isMetadataPoolEmpty(PhysicalSlotId(1)));
	io->sync();

	std::array<std::byte, RSS_ENVELOPE_BYTES> invalidEnvelope{};
	invalidEnvelope[0] = std::byte{0x01};
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, invalidEnvelope.data(), invalidEnvelope.size());
	io->sync();
	reopen();
	expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, initial)); },
					  FormatError::FERR_INVALID_MAGIC);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsAnUnpublishedPoolEraseAndInvalidPhysicalTail) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1)));
	allocator.abandonUnpublishedPool(PhysicalSlotId(1));
	EXPECT_TRUE(allocator.stats().poisoned);
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(1))); },
					  FormatError::FERR_INVALID_STATE);
	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);
	allocator.abandonUnpublishedPool(PhysicalSlotId{});
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);
	allocator.abandonUnpublishedPool(PhysicalSlotId(99));
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);

	const std::byte byte{0};
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES + RSS_SEGMENT_SIZE, &byte, sizeof(byte));
	io->sync();
	for (uint32_t index = 0; index < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
		static_cast<void>(allocator.allocatePage(Generation(1)));
	}
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(1))); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsEveryPoolEnvelopeIdentityMismatch) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1)));
	io->sync();

	const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES;
	std::array<std::byte, RSS_ENVELOPE_BYTES> bytes{};
	ASSERT_EQ(io->readAt(poolOffset, bytes.data(), bytes.size()), bytes.size());
	const SegmentEnvelope original = SegmentEnvelopeCodec::decode(bytes);
	auto expectInvalidEnvelope = [&](const SegmentEnvelope &invalid) {
		const auto encoded = SegmentEnvelopeCodec::encode(invalid);
		io->writeAt(poolOffset, encoded.data(), encoded.size());
		io->sync();
		expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, initial)); },
						  FormatError::FERR_INVALID_HEADER);
		const auto restored = SegmentEnvelopeCodec::encode(original);
		io->writeAt(poolOffset, restored.data(), restored.size());
		io->sync();
	};

	SegmentEnvelope invalid = original;
	invalid.state = SegmentState::SGS_BUILDING;
	expectInvalidEnvelope(invalid);
	invalid = original;
	invalid.segment_id = SegmentId(2);
	expectInvalidEnvelope(invalid);
	invalid = original;
	invalid.owner_id = 2U;
	expectInvalidEnvelope(invalid);
	invalid = original;
	invalid.header_bytes = RSS_ENVELOPE_BYTES;
	expectInvalidEnvelope(invalid);
	invalid = original;
	invalid.payload_bytes = 0;
	expectInvalidEnvelope(invalid);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsPoolHeadersForTheWrongPhysicalSlot) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1)));
	io->sync();

	const uint64_t headerOffset = RSS_FILE_BOOTSTRAP_BYTES + RSS_ENVELOPE_BYTES;
	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> bytes{};
	ASSERT_EQ(io->readAt(headerOffset, bytes.data(), bytes.size()), bytes.size());
	MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(bytes);
	header.physical_slot = PhysicalSlotId(2);
	bytes = MetadataPagePoolHeaderCodec::encode(header);
	writeControlHeaderCopies(*io, RSS_FILE_BOOTSTRAP_BYTES, header);
	io->sync();

	expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, initial)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPagePoolTest, CatalogTreeFailureReturnsUnpublishedPoolFrames) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
														 LogSequenceNumber(2),
														 BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(allocator.stats().allocated_pages, 0U);
	EXPECT_FALSE(allocator.stats().poisoned);
	EXPECT_EQ(bootstrap.recover().catalog_root, PhysicalPageRef{});
}

TEST_F(RelocatableMetadataPagePoolTest, ValidatesPublishedReachabilityAndFailsClosedForCorruptedCandidates) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 2, 2)}, LogSequenceNumber(2));

	EXPECT_NO_THROW(allocator.validateReachability(published));
	const CatalogRadixPage originalRoot = pages.readCatalogRadix(published.catalog_root);
	CatalogRadixPage corruptedRoot = originalRoot;
	const auto child = std::find_if(corruptedRoot.children.begin(), corruptedRoot.children.end(),
									[](PhysicalPageRef reference) { return reference.isValid(); });
	ASSERT_NE(child, corruptedRoot.children.end());
	*child = PhysicalPageRef{PhysicalSlotId(99), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	pages.writeCatalogRadix(published.catalog_root, corruptedRoot);

	expectFormatError([&] { allocator.validateReachability(published); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);

	pages.writeCatalogRadix(published.catalog_root, originalRoot);
	EXPECT_NO_THROW(allocator.recover(published));
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsRepeatedLogicalMetadataPageIdsDuringReachabilityValidation) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 2, 2)}, LogSequenceNumber(2));

	const CatalogRadixPage root = pages.readCatalogRadix(published.catalog_root);
	const auto child = std::find_if(root.children.begin(), root.children.end(),
									[](PhysicalPageRef reference) { return reference.isValid(); });
	ASSERT_NE(child, root.children.end());
	CatalogLeafPage leaf = pages.readCatalogLeaf(*child);
	leaf.page_id = root.page_id;
	pages.writeCatalogLeaf(*child, leaf);

	expectFormatError([&] { allocator.validateReachability(published); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);

	leaf.page_id = MetadataPageId(root.page_id.value() + 1U);
	pages.writeCatalogLeaf(*child, leaf);
	EXPECT_NO_THROW(allocator.recover(published));
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, BindsAndRegistersCatalogPoolIdentityOnlyForAllocatedPrivatePools) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2));
	ASSERT_EQ(allocated.physical_slot, PhysicalSlotId(1));
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId{}, Generation(2),
											  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(1U), Generation{},
											  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(1U), Generation(2),
											  LogSequenceNumber{});
			},
			FormatError::FERR_INVALID_HEADER);

	const auto expectInvalidBindingThenRecover = [&](SegmentId segmentId, Generation generation,
													 LogSequenceNumber lsn) {
		expectFormatError(
				[&] { allocator.bindCatalogPoolIdentity(allocated.physical_slot, segmentId, generation, lsn); },
				FormatError::FERR_INVALID_HEADER);
		EXPECT_TRUE(allocator.stats().poisoned);
		EXPECT_NO_THROW(allocator.recover(initial));
		EXPECT_FALSE(allocator.stats().poisoned);
		static_cast<void>(allocator.allocatePage(Generation(2)));
	};
	expectInvalidBindingThenRecover(SegmentId{}, Generation(2), LogSequenceNumber(1));
	expectInvalidBindingThenRecover(SegmentId(1U), Generation{}, LogSequenceNumber(1));
	expectInvalidBindingThenRecover(SegmentId(1U), Generation(2), LogSequenceNumber{});
	const auto expectNonPrivateBindingThenRecover = [&](PhysicalSlotId slot, Generation generation,
														LogSequenceNumber lsn) {
		expectFormatError([&] { allocator.bindCatalogPoolIdentity(slot, SegmentId(1U), generation, lsn); },
						  FormatError::FERR_INVALID_STATE);
		EXPECT_TRUE(allocator.stats().poisoned);
		EXPECT_NO_THROW(allocator.recover(initial));
		EXPECT_FALSE(allocator.stats().poisoned);
		static_cast<void>(allocator.allocatePage(Generation(2)));
	};
	expectNonPrivateBindingThenRecover(PhysicalSlotId(9U), Generation(2), LogSequenceNumber(1));
	expectNonPrivateBindingThenRecover(allocated.physical_slot, Generation(3), LogSequenceNumber(1));
	expectNonPrivateBindingThenRecover(allocated.physical_slot, Generation(2), LogSequenceNumber(2));

	allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(71), Generation(2), LogSequenceNumber(1));
	EXPECT_EQ(allocator.poolHeader(allocated.physical_slot).segment_id, SegmentId(71));
	EXPECT_NO_THROW(
			allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71), Generation(2), LogSequenceNumber(1)));
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(PhysicalSlotId{}, SegmentId(71), Generation(2), LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_HEADER);

	allocator.abandonPage(allocated);
	expectFormatError(
			[&] {
				allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(72), Generation(2),
												  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_NO_THROW(allocator.recover(initial));
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsCatalogPoolIdentityThatDivergesBetweenMemoryAndDisk) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2));

	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(99U), Generation(2),
											  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_HEADER);
	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	envelope.segment_id = SegmentId(81);
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size());

	MetadataPagePoolHeader header = allocator.poolHeader(allocated.physical_slot);
	header.segment_id = envelope.segment_id;
	writeControlHeaderCopies(*io, RSS_FILE_BOOTSTRAP_BYTES, header);
	io->sync();

	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(81), Generation(2),
											  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, PoisonsIdentityBindingWhenThePoolEnvelopeIsCorrupt) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2));

	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	envelope.owner_id = allocated.physical_slot.value() + 1U;
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, envelopeBytes.data(), envelopeBytes.size());
	io->sync();

	expectFormatError(
			[&] {
				allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(82), Generation(2),
												  LogSequenceNumber(1));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, ProtectsEmergencyReserveAndReleasesOnlyEmptyCatalogPools) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());
	EXPECT_FALSE(allocator.isEmergencyReserveSlot(PhysicalSlotId(1U)));
	expectFormatError([&] { allocator.createEmergencyReserve(MetadataEmergencyReserveDescriptor{}); },
					  FormatError::FERR_INVALID_STATE);
	// The bootstrap slot is occupied first, so the reserve is a distinct valid tail slot.
	static_cast<void>(allocator.allocatePage(Generation(1)));
	MetadataEmergencyReserveDescriptor reserve;
	reserve.physical_slot = PhysicalSlotId(2);
	reserve.pool_token = SegmentId(71);
	reserve.generation = Generation(1);
	reserve.committed_lsn = LogSequenceNumber(1);

	allocator.createEmergencyReserve(reserve);
	EXPECT_EQ(allocator.emergencyReserve(), reserve);
	EXPECT_TRUE(allocator.isEmergencyReserveSlot(reserve.physical_slot));
	EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());
	EXPECT_EQ(allocator.poolHeader(reserve.physical_slot).segment_id, reserve.pool_token);
	expectFormatError([&] { allocator.createEmergencyReserve(reserve); }, FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef catalogPage =
			allocator.allocatePage(Generation(2), LogSequenceNumber(2), std::array{PhysicalSlotId(1)}).page;
	EXPECT_EQ(catalogPage.physical_slot, PhysicalSlotId(3));
	EXPECT_FALSE(allocator.isEmergencyReserveSlot(catalogPage.physical_slot));
	expectFormatError([&] { allocator.releaseEmptyCatalogPools(std::array{reserve.physical_slot}); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { allocator.releaseEmptyCatalogPools(std::array{catalogPage.physical_slot}); },
					  FormatError::FERR_INVALID_STATE);

	allocator.abandonPage(catalogPage);
	EXPECT_TRUE(allocator.isMetadataPoolEmpty(catalogPage.physical_slot));
	EXPECT_NO_THROW(allocator.releaseEmptyCatalogPools(std::array{catalogPage.physical_slot}));
	EXPECT_FALSE(allocator.isMetadataPoolSlot(catalogPage.physical_slot));
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsEmergencyReserveThatOverlapsAnExistingPool) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	for (uint32_t frame = 0U; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		static_cast<void>(allocator.allocatePage(Generation(1U)));
	}
	MetadataEmergencyReserveDescriptor occupiedReserve;
	occupiedReserve.physical_slot = PhysicalSlotId(2U);
	occupiedReserve.pool_token = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
	occupiedReserve.generation = Generation(1U);
	occupiedReserve.committed_lsn = LogSequenceNumber(1U);
	expectFormatError([&] { allocator.createEmergencyReserve(occupiedReserve); }, FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.emergencyReserve().isValid());
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsInvalidReservePlacementAndUnorderedCatalogPoolRelease) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef first = allocator.allocatePage(Generation(1U));
	ASSERT_EQ(first.physical_slot, PhysicalSlotId(1U));

	MetadataEmergencyReserveDescriptor skippedTailReserve;
	skippedTailReserve.physical_slot = PhysicalSlotId(3U);
	skippedTailReserve.pool_token = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
	skippedTailReserve.generation = Generation(1U);
	skippedTailReserve.committed_lsn = LogSequenceNumber(1U);
	ASSERT_TRUE(skippedTailReserve.isValid());
	expectFormatError([&] { allocator.createEmergencyReserve(skippedTailReserve); }, FormatError::FERR_INVALID_STATE);

	MetadataEmergencyReserveDescriptor reserve = skippedTailReserve;
	reserve.physical_slot = PhysicalSlotId(2U);
	allocator.createEmergencyReserve(reserve);
	const PhysicalPageRef releasable = allocator
											   .allocatePage(Generation(1U), LogSequenceNumber(1U),
															 std::array{first.physical_slot, reserve.physical_slot})
											   .page;
	ASSERT_EQ(releasable.physical_slot, PhysicalSlotId(3U));
	allocator.abandonPage(releasable);

	expectFormatError(
			[&] { allocator.releaseEmptyCatalogPools(std::array{releasable.physical_slot, first.physical_slot}); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.isMetadataPoolSlot(releasable.physical_slot));
	expectFormatError([&] { allocator.releaseEmptyCatalogPools(std::array{PhysicalSlotId(4U)}); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_NO_THROW(allocator.releaseEmptyCatalogPools(std::array{releasable.physical_slot}));
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsOverflowingPoolLocationsAndReleasesEmptyPoolsWithoutAReserve) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(PhysicalSlotId(std::numeric_limits<uint64_t>::max()), SegmentId(1U),
											  Generation(1U), LogSequenceNumber(1U));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	const PhysicalPageRef page = allocator.allocatePage(Generation(1U));
	allocator.abandonPage(page);
	EXPECT_NO_THROW(allocator.releaseEmptyCatalogPools(std::array{page.physical_slot}));
	EXPECT_FALSE(allocator.isMetadataPoolSlot(page.physical_slot));
	expectFormatError([&] { allocator.releaseEmptyCatalogPools(std::array{PhysicalSlotId{}}); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsUnavailableAndDuplicatePhysicalFrames) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const CatalogTreePages layout = writeSingleLeafTree(initial, pages, allocator, SegmentId(1), Generation(1),
														LogSequenceNumber(1), Generation(1), LogSequenceNumber(1), 0);
	const CatalogRadixPage originalRoot = pages.readCatalogRadix(layout.root);

	CatalogRadixPage unavailable = originalRoot;
	unavailable.children[0] = PhysicalPageRef{PhysicalSlotId(2), 1, Generation(1)};
	pages.writeCatalogRadix(layout.root, unavailable);
	expectFormatError([&] { allocator.recover(layout.superblock); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);

	pages.writeCatalogRadix(layout.root, originalRoot);
	allocator.recover(layout.superblock);
	EXPECT_FALSE(allocator.stats().poisoned);

	CatalogRadixPage duplicate = originalRoot;
	duplicate.children[1] = layout.leaf;
	pages.writeCatalogRadix(layout.root, duplicate);
	expectFormatError([&] { allocator.recover(layout.superblock); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsRootKindMismatch) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const CatalogTreePages layout = writeSingleLeafTree(initial, pages, allocator, SegmentId(1), Generation(1),
														LogSequenceNumber(1), Generation(1), LogSequenceNumber(1), 0);
	CatalogRadixPage root = pages.readCatalogRadix(layout.root);
	root.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	pages.writeCatalogRadix(layout.root, root);

	expectFormatError([&] { allocator.recover(layout.superblock); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsRootGenerationAndLsnBeyondTheSuperblock) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef futureRoot = allocator.allocatePage(Generation(2));
	CatalogRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0, Generation(2),
									  LogSequenceNumber(1));
	pages.writeCatalogRadix(futureRoot, root);
	Superblock published = initial;
	published.catalog_root = futureRoot;
	expectFormatError([&] { allocator.recover(published); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);

	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);
	const PhysicalPageRef futureLsnRoot = allocator.allocatePage(Generation(1));
	root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0, Generation(1),
					 LogSequenceNumber(2));
	pages.writeCatalogRadix(futureLsnRoot, root);
	published.catalog_root = futureLsnRoot;
	expectFormatError([&] { allocator.recover(published); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsInternalLevelsThatDoNotMatchTheRootRoute) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef internalReference = allocator.allocatePage(Generation(1));
	CatalogRadixPage internal = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_INTERNAL, 1,
										  Generation(1), LogSequenceNumber(1));
	pages.writeCatalogRadix(internalReference, internal);
	const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
	CatalogRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 1, Generation(1),
									  LogSequenceNumber(1));
	root.children[0] = internalReference;
	pages.writeCatalogRadix(rootReference, root);
	Superblock published = initial;
	published.catalog_root = rootReference;

	expectFormatError([&] { allocator.recover(published); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsLeafGenerationLsnAndRouteViolations) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const CatalogTreePages futureLeaf =
			writeSingleLeafTree(initial, pages, allocator, SegmentId(1), Generation(1), LogSequenceNumber(1),
								Generation(2), LogSequenceNumber(1), 0);
	expectFormatError([&] { allocator.recover(futureLeaf.superblock); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsLeafLsnAndRadixRouteViolations) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const CatalogTreePages futureLeaf =
			writeSingleLeafTree(initial, pages, allocator, SegmentId(1), Generation(1), LogSequenceNumber(1),
								Generation(1), LogSequenceNumber(2), 0);
	expectFormatError([&] { allocator.recover(futureLeaf.superblock); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsLeavesOutsideTheirRadixRoute) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const CatalogTreePages layout =
			writeSingleLeafTree(initial, pages, allocator, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U), Generation(1),
								LogSequenceNumber(1), Generation(1), LogSequenceNumber(1), 0);

	expectFormatError([&] { allocator.recover(layout.superblock); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryHandlesMetadataPageIdExhaustionAndRejectsOverflowingRoutes) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const CatalogTreePages layout = writeSingleLeafTree(initial, pages, allocator, SegmentId(1), Generation(1),
														LogSequenceNumber(1), Generation(1), LogSequenceNumber(1), 0);
	CatalogRadixPage root = pages.readCatalogRadix(layout.root);
	root.page_id = MetadataPageId(std::numeric_limits<uint64_t>::max() - 1U);
	pages.writeCatalogRadix(layout.root, root);
	allocator.recover(layout.superblock);
	EXPECT_EQ(allocator.allocateMetadataPageId(), MetadataPageId(std::numeric_limits<uint64_t>::max()));
	expectFormatError([&] { static_cast<void>(allocator.allocateMetadataPageId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	root.page_id = MetadataPageId(std::numeric_limits<uint64_t>::max());
	pages.writeCatalogRadix(layout.root, root);
	allocator.recover(layout.superblock);
	expectFormatError([&] { static_cast<void>(allocator.allocateMetadataPageId()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsRadixRoutesThatOverflowTheSegmentIdDomain) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
	CatalogRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT,
									  RSS_CATALOG_RADIX_MAX_LEVEL, Generation(1), LogSequenceNumber(1));
	root.children[2] = rootReference;
	pages.writeCatalogRadix(rootReference, root);
	Superblock published = initial;
	published.catalog_root = rootReference;

	expectFormatError([&] { allocator.recover(published); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, DetectsTreeReferencesThatAreAbsentFromThePoolBitmap) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const uint64_t rootPoolHeaderOffset = RSS_FILE_BOOTSTRAP_BYTES + RSS_ENVELOPE_BYTES;
	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> bytes{};
	ASSERT_EQ(io->readAt(rootPoolHeaderOffset, bytes.data(), bytes.size()), bytes.size());
	MetadataPagePoolHeader header = MetadataPagePoolHeaderCodec::decode(bytes);
	header.allocated_frames = 0;
	bytes = MetadataPagePoolHeaderCodec::encode(header);
	writeControlHeaderCopies(*io, RSS_FILE_BOOTSTRAP_BYTES, header);
	io->sync();

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, recovered)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(recovered.generation, published.generation);
	EXPECT_EQ(recovered.checkpoint_lsn, published.checkpoint_lsn);
	EXPECT_EQ(recovered.catalog_root, published.catalog_root);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoversIndirectRootsAndRejectsMalformedRadixKinds) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const uint64_t highSegmentId = static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock first =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const Superblock second =
			tree.upsertAndPublish(bootstrap, first, std::vector{entry(highSegmentId, 101, 1, 3)}, LogSequenceNumber(3));
	EXPECT_EQ(allocator.stats().allocated_pages, 6U);
	allocator.recover(second);
	EXPECT_EQ(allocator.stats().allocated_pages, 5U);

	const CatalogRadixPage root = pages.readCatalogRadix(second.catalog_root);
	CatalogRadixPage malformed = pages.readCatalogRadix(root.children[1]);
	malformed.kind = CatalogRadixPageKind::CRPK_ROOT;
	pages.writeCatalogRadix(root.children[1], malformed);
	expectFormatError([&] { allocator.recover(second); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RootRecoveryTraversesEveryTypedLocationDirectoryAndRejectsAliasing) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	Superblock roots = initial;
	const std::array directoryTypes{
			MetadataDirectoryType::MDT_PAGE,
			MetadataDirectoryType::MDT_EXTENT,
			MetadataDirectoryType::MDT_NODE_ENTITY,
			MetadataDirectoryType::MDT_EDGE_ENTITY,
			MetadataDirectoryType::MDT_ENTITY_PROPERTY,
			MetadataDirectoryType::MDT_INDEX_ROOT,
			MetadataDirectoryType::MDT_VECTOR_LOCATION,
			MetadataDirectoryType::MDT_VECTOR_MANIFEST,
			MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST,
			MetadataDirectoryType::MDT_STATE,
			MetadataDirectoryType::MDT_RETIRE_QUEUE,
	};
	for (const MetadataDirectoryType directoryType: directoryTypes) {
		static_cast<void>(writeLocationDirectoryRoot(pages, allocator, directoryType, roots));
	}

	EXPECT_NO_THROW(allocator.recover(roots));
	EXPECT_EQ(allocator.stats().allocated_pages, directoryTypes.size() * 2U);
	EXPECT_NO_THROW(MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER));

	const MetadataRadixPage original = pages.readMetadataRadix(roots.page_directory_root);
	MetadataRadixPage wrongType = original;
	wrongType.directory_type = MetadataDirectoryType::MDT_EXTENT;
	pages.writeMetadataRadix(roots.page_directory_root, wrongType);
	expectFormatError(
			[&] {
				static_cast<void>(
						MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY));
			},
			FormatError::FERR_INVALID_HEADER);
	pages.writeMetadataRadix(roots.page_directory_root, original);

	roots.extent_directory_root = roots.page_directory_root;
	expectFormatError(
			[&] {
				static_cast<void>(
						MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPagePoolTest, RootRecoveryRejectsReachablePoolEnvelopeCorruptionAndReserveAliasing) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		static_cast<void>(allocator.allocatePage(Generation(1U)));
	}
	Superblock roots = initial;
	const PhysicalPageRef root = writeLocationDirectoryRoot(pages, allocator, MetadataDirectoryType::MDT_PAGE, roots);
	ASSERT_TRUE(root.isValid());
	ASSERT_GT(root.physical_slot.value(), 1U);
	const uint64_t rootPoolOffset =
			RSS_FILE_BOOTSTRAP_BYTES + (root.physical_slot.value() - 1U) * static_cast<uint64_t>(RSS_SEGMENT_SIZE);

	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(rootPoolOffset, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	const SegmentEnvelope originalEnvelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	SegmentEnvelope invalidEnvelope = originalEnvelope;
	invalidEnvelope.owner_id = invalidEnvelope.owner_id + 1U;
	envelopeBytes = SegmentEnvelopeCodec::encode(invalidEnvelope);
	io->writeAt(rootPoolOffset, envelopeBytes.data(), envelopeBytes.size());
	io->sync();
	expectFormatError(
			[&] {
				static_cast<void>(
						MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY));
			},
			FormatError::FERR_INVALID_HEADER);

	MetadataPagePoolHeader reserveHeader = allocator.poolHeader(root.physical_slot);
	reserveHeader.segment_id = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
	writeControlHeaderCopies(*io, rootPoolOffset, reserveHeader);
	SegmentEnvelope reserveEnvelope = originalEnvelope;
	reserveEnvelope.segment_id = reserveHeader.segment_id;
	envelopeBytes = SegmentEnvelopeCodec::encode(reserveEnvelope);
	io->writeAt(rootPoolOffset, envelopeBytes.data(), envelopeBytes.size());
	MetadataEmergencyReserveDescriptor aliasedReserve;
	aliasedReserve.physical_slot = root.physical_slot;
	aliasedReserve.pool_token = reserveEnvelope.segment_id;
	aliasedReserve.generation = reserveEnvelope.generation;
	aliasedReserve.committed_lsn = reserveEnvelope.committed_lsn;
	ASSERT_TRUE(aliasedReserve.isValid());
	roots.metadata_emergency_reserve = aliasedReserve;
	io->sync();
	expectFormatError(
			[&] {
				static_cast<void>(
						MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsTruncatedAndCorruptPoolHeaders) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(2)));
	io->sync();
	std::byte corrupt{};
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES + RSS_ENVELOPE_BYTES, &corrupt, sizeof(corrupt));
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES + RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES, &corrupt,
					sizeof(corrupt));
	io->sync();
	reopen();
	expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, initial)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsDurablePoolBitmapDivergenceBeforeClearingPoison) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef page = allocator.allocatePage(Generation(1U));
	ASSERT_TRUE(page.isValid());

	MetadataPagePoolHeader durableHeader = allocator.poolHeader(page.physical_slot);
	durableHeader.allocated_frames = 0U;
	writeControlHeaderCopies(*io, RSS_FILE_BOOTSTRAP_BYTES, durableHeader);
	io->sync();

	expectFormatError([&] { allocator.recover(initial); }, FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), FormatException);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsInvalidAllocatorInputsAndMalformedPoolTails) {
	EXPECT_THROW(static_cast<void>(MetadataPagePoolAllocator(nullptr, superblock(1, 1))), std::invalid_argument);
	const Superblock initial = superblock(1, 1);
	EXPECT_THROW(MetadataPagePoolAllocator(io, initial, static_cast<MetadataPagePoolOpenMode>(0)),
				 std::invalid_argument);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation{})); },
					  FormatError::FERR_INVALID_HEADER);
	allocator.abandonPage(PhysicalPageRef{});
	EXPECT_TRUE(allocator.stats().poisoned);
	expectFormatError([&] { static_cast<void>(allocator.allocateMetadataPageId()); }, FormatError::FERR_INVALID_STATE);
	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);

	const std::byte tail{};
	io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, &tail, sizeof(tail));
	io->sync();
	reopen();
	expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(io, initial)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPagePoolTest, FailsClosedWhenDiskFullAlsoExhaustsTheEmergencyReserve) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			EXPECT_EQ(allocator.allocatePage(Generation(1U)).physical_slot, PhysicalSlotId(1U));
		}
		MetadataEmergencyReserveDescriptor reserve;
		reserve.physical_slot = PhysicalSlotId(2U);
		reserve.pool_token = SegmentId(71U);
		reserve.generation = Generation(1U);
		reserve.committed_lsn = LogSequenceNumber(1U);
		allocator.createEmergencyReserve(reserve);

		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnNextNormalPool(
				[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (offset >= static_cast<pwrite_off_t>(blockedPoolOffset) &&
						offset < static_cast<pwrite_off_t>(blockedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});

		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			EXPECT_EQ(allocator.allocatePage(Generation(1U)).physical_slot, reserve.physical_slot);
		}
		expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(1U))); },
						  FormatError::FERR_INVALID_STATE);
		EXPECT_TRUE(allocator.emergencyReserveHasAllocatedFrames());
		EXPECT_FALSE(allocator.stats().poisoned);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, DoesNotTreatUnexpectedNativeWriteFailureAsDiskFull) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}

		MetadataEmergencyReserveDescriptor reserve;
		reserve.physical_slot = PhysicalSlotId(2U);
		reserve.pool_token = SegmentId(72U);
		reserve.generation = Generation(1U);
		reserve.committed_lsn = LogSequenceNumber(1U);
		allocator.createEmergencyReserve(reserve);

		const uint64_t failedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		StorageIO::ScopedNativeWriteHookForTesting unexpectedWriteFailure(
				[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (offset >= static_cast<pwrite_off_t>(failedPoolOffset) &&
						offset < static_cast<pwrite_off_t>(failedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});

		EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), std::system_error);
		EXPECT_FALSE(allocator.stats().poisoned);
		EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, DoesNotFallbackToAnAbsentOrExcludedEmergencyReserve) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}
		const uint64_t firstBlockedOffset = RSS_FILE_BOOTSTRAP_BYTES + RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullWithoutReserve(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(firstBlockedOffset) &&
							offset < static_cast<pwrite_off_t>(firstBlockedOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), std::system_error);
		}

		MetadataEmergencyReserveDescriptor reserve;
		reserve.physical_slot = PhysicalSlotId(2U);
		reserve.pool_token = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
		reserve.generation = Generation(1U);
		reserve.committed_lsn = LogSequenceNumber(1U);
		allocator.createEmergencyReserve(reserve);
		const uint64_t secondBlockedOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullWithExcludedReserve(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(secondBlockedOffset) &&
							offset < static_cast<pwrite_off_t>(secondBlockedOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U), LogSequenceNumber(1U),
																  std::array{reserve.physical_slot})),
						 std::system_error);
		}
		EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());
		EXPECT_FALSE(allocator.stats().poisoned);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RotatesAnEmergencyReserveOnlyAfterItServesDiskFullFallback) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}
		MetadataEmergencyReserveDescriptor reserve;
		reserve.physical_slot = PhysicalSlotId(2U);
		reserve.pool_token = SegmentId(73U);
		reserve.generation = Generation(1U);
		reserve.committed_lsn = LogSequenceNumber(1U);
		allocator.createEmergencyReserve(reserve);

		expectFormatError(
				[&] {
					static_cast<void>(
							allocator.rotateEmergencyReserve(SegmentId(74U), Generation(2U), LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);

		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnNextNormalPool(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(blockedPoolOffset) &&
							offset < static_cast<pwrite_off_t>(blockedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			const PhysicalPageRef fallback = allocator.allocatePage(Generation(1U));
			EXPECT_EQ(fallback.physical_slot, reserve.physical_slot);
		}
		EXPECT_TRUE(allocator.emergencyReserveHasAllocatedFrames());

		expectFormatError(
				[&] {
					static_cast<void>(allocator.rotateEmergencyReserve(reserve.pool_token, Generation(2U),
																	   LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					static_cast<void>(
							allocator.rotateEmergencyReserve(SegmentId{}, Generation(2U), LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					static_cast<void>(
							allocator.rotateEmergencyReserve(SegmentId(74U), Generation{}, LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					static_cast<void>(
							allocator.rotateEmergencyReserve(SegmentId(74U), Generation(2U), LogSequenceNumber{}));
				},
				FormatError::FERR_INVALID_STATE);
		const uint64_t pristineTail = nativeIo->fileSize();
		const std::byte corruptTail{};
		nativeIo->writeAt(pristineTail, &corruptTail, sizeof(corruptTail));
		expectFormatError(
				[&] {
					static_cast<void>(
							allocator.rotateEmergencyReserve(SegmentId(74U), Generation(2U), LogSequenceNumber(2U)));
				},
				FormatError::FERR_ARITHMETIC_OVERFLOW);
		nativeIo->truncate(pristineTail);
		const MetadataEmergencyReserveDescriptor successor =
				allocator.rotateEmergencyReserve(SegmentId(74U), Generation(2U), LogSequenceNumber(2U));
		EXPECT_EQ(successor.physical_slot, PhysicalSlotId(3U));
		EXPECT_EQ(allocator.emergencyReserve(), successor);
		EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());

		allocator.restoreEmergencyReserve(reserve, successor);
		EXPECT_EQ(allocator.emergencyReserve(), reserve);
		allocator.restoreEmergencyReserve(MetadataEmergencyReserveDescriptor{}, successor);
		EXPECT_EQ(allocator.emergencyReserve(), reserve);
		allocator.restoreEmergencyReserve(reserve, successor);
		EXPECT_EQ(allocator.emergencyReserve(), reserve);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, ReclaimsAnEmptyUnpublishedTailPoolWithNativeTruncation) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		const PhysicalPageRef page = allocator.allocatePage(Generation(1U));
		allocator.abandonPage(page);
		EXPECT_EQ(allocator.physicalFileSize(), RSS_FILE_BOOTSTRAP_BYTES + RSS_SEGMENT_SIZE);

		allocator.abandonUnpublishedPool(page.physical_slot);
		EXPECT_FALSE(allocator.isMetadataPoolSlot(page.physical_slot));
		EXPECT_EQ(allocator.physicalFileSize(), RSS_FILE_BOOTSTRAP_BYTES);
		EXPECT_FALSE(allocator.stats().poisoned);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RemovesUnpublishedPoolWithoutNativeTailTruncation) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef page = allocator.allocatePage(Generation(1U));
	allocator.abandonPage(page);
	allocator.abandonUnpublishedPool(page.physical_slot);

	EXPECT_FALSE(allocator.isMetadataPoolSlot(page.physical_slot));
	EXPECT_EQ(allocator.physicalFileSize(), RSS_FILE_BOOTSTRAP_BYTES + RSS_SEGMENT_SIZE);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, TruncateFailureKeepsTheAllocatorPoisonedUntilFreshRecovery) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(nativeIo, initial);
	const PhysicalPageRef page = allocator.allocatePage(Generation(1U));
	allocator.abandonPage(page);

	{
		StorageIO::ScopedNativeTruncateHookForTesting failTruncate([](file_handle_t, uint64_t) {
#ifdef _WIN32
			SetLastError(ERROR_WRITE_FAULT);
#else
			errno = EIO;
#endif
			return -1;
		});
		allocator.abandonUnpublishedPool(page.physical_slot);
	}
	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), FormatException);
	expectFormatError([&] { allocator.recover(initial); }, FormatError::FERR_CHECKSUM_MISMATCH);
	EXPECT_TRUE(allocator.stats().poisoned);

	// A new allocator reconstructs from the durable file, ignores the zeroed
	// unpublished tail, and never reuses the stale in-memory pool identity.
	MetadataPagePoolAllocator reopened(nativeIo, initial);
	EXPECT_EQ(reopened.stats().pools, 0U);
	EXPECT_FALSE(reopened.stats().poisoned);

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, PoisonsInvalidUnpublishedBatchCleanupBeforeAnyReuse) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const std::array invalidSlots{PhysicalSlotId{}};

	allocator.abandonUnpublishedBatch(invalidSlots, {}, RSS_FILE_BOOTSTRAP_BYTES);
	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), FormatException);

	EXPECT_NO_THROW(allocator.recover(initial));
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, BatchCleanupDoesNotMaterializeAnUnwrittenDataReservation) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const uint64_t initialFileSize = io->fileSize();

	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
		batch.reserveDataSlot(PhysicalSlotId(1U));
	}

	EXPECT_EQ(io->fileSize(), initialFileSize);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, BatchCleanupWipesAMaterializedDataReservationWithoutTailTruncation) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const uint64_t initialFileSize = io->fileSize();

	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
		batch.reserveDataSlot(PhysicalSlotId(1U));
		const std::byte sentinel{0x4D};
		io->writeAt(initialFileSize + RSS_SEGMENT_SIZE - 1U, &sentinel, sizeof(sentinel));
	}

	std::byte cleared{0xFF};
	ASSERT_EQ(io->readAt(initialFileSize + RSS_SEGMENT_SIZE - 1U, &cleared, sizeof(cleared)), sizeof(cleared));
	EXPECT_EQ(cleared, std::byte{});
	EXPECT_EQ(io->fileSize(), initialFileSize + RSS_SEGMENT_SIZE);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, BatchCleanupPoisonsUnknownTrackedPoolBeforeReuse) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	{
		MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
		batch.trackUnpublishedPool(PhysicalSlotId(1U));
	}

	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), FormatException);
}

TEST_F(RelocatableMetadataPagePoolTest, RegisterCatalogPoolRejectsConflictingInMemoryIdentity) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2U));
	allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(71U), Generation(2U), LogSequenceNumber(1U));

	const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES;
	std::array<std::byte, RSS_ENVELOPE_BYTES> envelopeBytes{};
	ASSERT_EQ(io->readAt(poolOffset, envelopeBytes.data(), envelopeBytes.size()), envelopeBytes.size());
	SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(envelopeBytes);
	envelope.generation = Generation(3U);
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(poolOffset, envelopeBytes.data(), envelopeBytes.size());

	MetadataPagePoolHeader header = allocator.poolHeader(allocated.physical_slot);
	header.generation = Generation(3U);
	writeControlHeaderCopies(*io, poolOffset, header);
	io->sync();

	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71U), Generation(3U),
											  LogSequenceNumber(1U));
			},
			FormatError::FERR_INVALID_STATE);

	envelope.generation = Generation(2U);
	envelope.committed_lsn = LogSequenceNumber(2U);
	envelopeBytes = SegmentEnvelopeCodec::encode(envelope);
	io->writeAt(poolOffset, envelopeBytes.data(), envelopeBytes.size());
	header.generation = Generation(2U);
	header.committed_lsn = LogSequenceNumber(2U);
	writeControlHeaderCopies(*io, poolOffset, header);
	io->sync();
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71U), Generation(2U),
											  LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RegisterCatalogPoolRejectsMismatchedDurableGenerationAndLsn) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2U));
	allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(71U), Generation(2U), LogSequenceNumber(1U));

	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71U), Generation(3U),
											  LogSequenceNumber(1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71U), Generation(2U),
											  LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				allocator.bindCatalogPoolIdentity(PhysicalSlotId{}, SegmentId(71U), Generation(2U),
												  LogSequenceNumber(1U));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);
	EXPECT_FALSE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsNoncanonicalAllocatorSummaryAndCatalogLeaves) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef allocatorLeafReference = allocator.allocatePage(Generation(1U));
	AllocatorLeafPage allocatorLeaf;
	allocatorLeaf.page_id = allocator.allocateMetadataPageId();
	allocatorLeaf.first_slot = PhysicalSlotId(1U);
	allocatorLeaf.slot_count = 8U;
	allocatorLeaf.generation = Generation(1U);
	allocatorLeaf.committed_lsn = LogSequenceNumber(1U);
	pages.writeAllocatorLeaf(allocatorLeafReference, allocatorLeaf);

	const PhysicalPageRef allocatorRootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage allocatorRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0U,
												Generation(1U), LogSequenceNumber(1U));
	allocatorRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	allocatorRoot.children[0] = allocatorLeafReference;
	Superblock allocatorRoots = initial;
	allocatorRoots.allocator_root = allocatorRootReference;

	pages.writeMetadataRadix(allocatorRootReference, allocatorRoot);
	expectFormatError([&] { allocator.recover(allocatorRoots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);

	const PhysicalPageRef emptyAllocatorRootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage emptyAllocatorRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0U,
													 Generation(1U), LogSequenceNumber(1U));
	emptyAllocatorRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	emptyAllocatorRoot.allocator_free_children[0] = std::byte{0x01};
	pages.writeMetadataRadix(emptyAllocatorRootReference, emptyAllocatorRoot);
	allocatorRoots.allocator_root = emptyAllocatorRootReference;
	expectFormatError([&] { allocator.recover(allocatorRoots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);

	const PhysicalPageRef catalogLeafReference = allocator.allocatePage(Generation(1U));
	CatalogLeafPage catalogLeaf;
	catalogLeaf.page_id = allocator.allocateMetadataPageId();
	catalogLeaf.generation = Generation(1U);
	catalogLeaf.committed_lsn = LogSequenceNumber(1U);
	pages.writeCatalogLeaf(catalogLeafReference, catalogLeaf);
	const PhysicalPageRef catalogRootReference = allocator.allocatePage(Generation(1U));
	CatalogRadixPage catalogRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0U,
											 Generation(1U), LogSequenceNumber(1U));
	catalogRoot.children[0] = catalogLeafReference;
	pages.writeCatalogRadix(catalogRootReference, catalogRoot);
	Superblock catalogRoots = initial;
	catalogRoots.catalog_root = catalogRootReference;
	expectFormatError([&] { allocator.recover(catalogRoots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsLocationLeafWithWrongDirectoryType) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	Superblock roots = initial;
	const PhysicalPageRef root = writeLocationDirectoryRoot(pages, allocator, MetadataDirectoryType::MDT_PAGE, roots);
	const MetadataRadixPage radix = pages.readMetadataRadix(root);
	LocationLeafPage leaf = pages.readLocationLeaf(radix.children[0]);
	leaf.directory_type = MetadataDirectoryType::MDT_EXTENT;
	pages.writeLocationLeaf(radix.children[0], leaf);

	expectFormatError([&] { allocator.recover(roots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}
