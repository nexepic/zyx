/**
 * @file test_MetadataPageFile.cpp
 * @brief Durable placement and reopen tests for relocatable Catalog metadata pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"

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

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		PhysicalPageRef result;
		result.physical_slot = PhysicalSlotId(slot);
		result.frame_index = frame;
		result.generation = Generation(generation);
		return result;
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn, PhysicalPageRef catalogRoot = {}) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = catalogRoot;
		return result;
	}

	CatalogEntry entry(uint64_t segmentId, uint64_t physicalSlot, uint64_t generation, uint64_t lsn) {
		CatalogEntry result;
		result.segment_id = SegmentId(segmentId);
		result.physical_slot = PhysicalSlotId(physicalSlot);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 99;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	CatalogLeafPage leafPage(Generation generation, LogSequenceNumber lsn) {
		CatalogLeafPage page;
		page.page_id = MetadataPageId(100);
		page.minimum_segment_id = SegmentId(1);
		page.maximum_segment_id = SegmentId(2);
		page.generation = generation;
		page.committed_lsn = lsn;
		page.entries = {entry(1, 20, 1, lsn.value()), entry(2, 21, 1, lsn.value())};
		return page;
	}

	AllocatorLeafPage freeSpaceLeaf(Generation generation, LogSequenceNumber lsn) {
		AllocatorLeafPage page;
		page.page_id = MetadataPageId(102);
		page.first_slot = PhysicalSlotId(1);
		page.slot_count = 10;
		page.generation = generation;
		page.committed_lsn = lsn;
		page.allocated_slots[0] = std::byte{0b00000011};
		return page;
	}

	LocationLeafPage locationLeaf(Generation generation, LogSequenceNumber lsn) {
		LocationLeafPage page;
		page.directory_type = MetadataDirectoryType::MDT_PAGE;
		page.page_id = MetadataPageId(103);
		page.generation = generation;
		page.committed_lsn = lsn;
		page.entries = {{1, SegmentId(22), Generation(1), 1, 1}};
		return page;
	}

	MetadataPageRouteLeafPage metadataRouteLeaf(Generation generation, LogSequenceNumber lsn) {
		MetadataPageRouteLeafPage page;
		page.page_id = MetadataPageId(104U);
		page.generation = generation;
		page.committed_lsn = lsn;
		page.routes = {{MetadataPageId(1U), pageRef(2U, 1U, generation.value())},
					   {MetadataPageId(7U), pageRef(2U, 2U, generation.value())}};
		return page;
	}

} // namespace

class RelocatableMetadataPageFileTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath =
				std::filesystem::temp_directory_path() / ("relocatable_metadata_page_" + to_string(uuid) + ".dat");
		openFile(std::ios::trunc);
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

	void reopen() {
		io.reset();
		file->close();
		file.reset();
		openFile({});
	}

	void openFile(std::ios::openmode extraMode) {
		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | extraMode);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}
};

TEST_F(RelocatableMetadataPageFileTest, PersistsRootAndLeafBeforePublishingTheBootstrapReference) {
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(superblock(1, 10));
	const PhysicalPageRef leafRef = pageRef(1, 1, 1);
	const PhysicalPageRef rootRef = pageRef(1, 2, 1);
	const Superblock candidate = superblock(2, 11, rootRef);
	MetadataPageFile pages(io, candidate);

	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.level = 0;
	root.page_id = MetadataPageId(101);
	root.generation = Generation(1);
	root.committed_lsn = LogSequenceNumber(11);
	root.children[0] = leafRef;

	pages.writeCatalogLeaf(leafRef, leafPage(Generation(1), LogSequenceNumber(11)));
	pages.writeCatalogRadix(rootRef, root);
	bootstrap.publish(candidate);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.catalog_root, rootRef);
	MetadataPageFile reopenedPages(io, recovered);
	const CatalogRadixPage loadedRoot = reopenedPages.readCatalogRadix(recovered.catalog_root);
	const PhysicalPageRef routedLeaf = CatalogRadixPageCodec::childFor(loadedRoot, SegmentId(1));
	EXPECT_EQ(routedLeaf, leafRef);
	const CatalogLeafPage loadedLeaf = reopenedPages.readCatalogLeaf(routedLeaf);
	ASSERT_EQ(loadedLeaf.entries.size(), 2U);
	EXPECT_EQ(loadedLeaf.entries[0].segment_id, SegmentId(1));
	EXPECT_EQ(loadedLeaf.entries[1].physical_slot, PhysicalSlotId(21));
	EXPECT_EQ(pages.pageOffset(leafRef), RSS_FILE_BOOTSTRAP_BYTES + RSS_PAGE_SIZE);
	EXPECT_EQ(pages.pageOffset(rootRef), RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_PAGE_SIZE);
}

TEST_F(RelocatableMetadataPageFileTest, UsesGenerationTaggedBufferPoolEntriesForCowMetadataPages) {
	const Superblock current = superblock(2, 20);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef oldRef = pageRef(1, 1, 1);
	const PhysicalPageRef replacementRef = pageRef(1, 2, 2);
	const CatalogLeafPage oldPage = leafPage(Generation(1), LogSequenceNumber(20));
	const CatalogLeafPage replacementPage = leafPage(Generation(2), LogSequenceNumber(21));

	io->enableBufferPool(4U * RSS_PAGE_SIZE);
	pages.writeCatalogLeaf(oldRef, oldPage);
	pages.writeCatalogLeaf(replacementRef, replacementPage);
	io->sync();
	io->resetBufferPoolStats();

	EXPECT_EQ(pages.readCatalogLeaf(oldRef).generation, Generation(1));
	EXPECT_EQ(pages.readCatalogLeaf(replacementRef).generation, Generation(2));
	const BufferPoolStats warmStats = io->bufferPoolStats();
	EXPECT_EQ(warmStats.misses, 0U);
	EXPECT_EQ(warmStats.hits, 2U);
	EXPECT_EQ(warmStats.resident_bytes, 2U * RSS_PAGE_SIZE);

	PhysicalPageRef staleRef = oldRef;
	staleRef.generation = Generation(3);
	expectFormatError([&] { static_cast<void>(pages.readCatalogLeaf(staleRef)); }, FormatError::FERR_INVALID_STATE);
	const BufferPoolStats staleStats = io->bufferPoolStats();
	EXPECT_EQ(staleStats.misses, 1U);
	EXPECT_EQ(staleStats.hits, 2U);
	EXPECT_EQ(staleStats.resident_bytes, 3U * RSS_PAGE_SIZE);

	// The old COW frame remains independently readable while the replacement is
	// resident, so a cache hit cannot substitute a newer metadata generation.
	EXPECT_EQ(pages.readCatalogLeaf(oldRef).generation, Generation(1));
	EXPECT_EQ(io->bufferPoolStats().hits, 3U);
}

TEST_F(RelocatableMetadataPageFileTest, ValidatesAndForwardsDemandReadHintsBeforeResolvingReferences) {
	const Superblock current = superblock(1, 1);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef leafRef = pageRef(1, 1, 1);
	const CatalogLeafPage expected = leafPage(Generation(1), LogSequenceNumber(1));
	pages.writeCatalogLeaf(leafRef, expected);
	io->sync();
	io->enableBufferPool(4U * RSS_PAGE_SIZE);
	io->resetBufferPoolStats();

	const CatalogLeafPage loaded = pages.readCatalogLeaf(leafRef, BufferPoolReadHint::BPRH_PREFETCH);
	ASSERT_EQ(loaded.entries.size(), expected.entries.size());
	EXPECT_EQ(loaded.entries.front().segment_id, expected.entries.front().segment_id);
	EXPECT_EQ(loaded.entries.back().segment_id, expected.entries.back().segment_id);
	const BufferPoolStats prefetched = io->bufferPoolStats();
	EXPECT_EQ(prefetched.misses, 1U);
	EXPECT_EQ(prefetched.disk_read_bytes, RSS_PAGE_SIZE);
	EXPECT_EQ(prefetched.resident_bytes, RSS_PAGE_SIZE);

	expectFormatError(
			[&] {
				static_cast<void>(pages.readCatalogLeaf(
						PhysicalPageRef{}, static_cast<BufferPoolReadHint>(std::numeric_limits<uint8_t>::max())));
			},
			FormatError::FERR_INVALID_ENUM);
	EXPECT_EQ(io->bufferPoolStats().misses, prefetched.misses);
}

TEST_F(RelocatableMetadataPageFileTest, PersistsTypedMetadataRoutesWithoutUsingLocationLeaves) {
	const Superblock current = superblock(2U, 2U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef routeReference = pageRef(1U, 1U, 2U);
	const MetadataPageRouteLeafPage expected = metadataRouteLeaf(Generation(2U), LogSequenceNumber(2U));
	pages.writeMetadataPageRouteLeaf(routeReference, expected);
	io->sync();
	EXPECT_EQ(pages.readMetadataPageRouteLeaf(routeReference).routes, expected.routes);
	expectFormatError([&] { static_cast<void>(pages.readLocationLeaf(routeReference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	MetadataPageRouteLeafPage wrongGeneration = expected;
	wrongGeneration.generation = Generation(1U);
	expectFormatError([&] { pages.writeMetadataPageRouteLeaf(routeReference, wrongGeneration); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, AuthenticatesTypedRoutedPagesBeforeLocalRelocation) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef catalogReference = pageRef(1U, 1U, 1U);
	const PhysicalPageRef allocatorReference = pageRef(1U, 2U, 1U);
	const PhysicalPageRef locationReference = pageRef(1U, 3U, 1U);
	const PhysicalPageRef routeReference = pageRef(1U, 4U, 1U);
	const CatalogLeafPage catalog = leafPage(Generation(1U), LogSequenceNumber(1U));
	const AllocatorLeafPage allocator = freeSpaceLeaf(Generation(1U), LogSequenceNumber(1U));
	const LocationLeafPage location = locationLeaf(Generation(1U), LogSequenceNumber(1U));

	pages.writeCatalogLeaf(catalogReference, catalog);
	pages.writeAllocatorLeaf(allocatorReference, allocator);
	pages.writeLocationLeaf(locationReference, location);
	pages.writeMetadataPageRouteLeaf(routeReference, metadataRouteLeaf(Generation(1U), LogSequenceNumber(1U)));
	io->sync();

	EXPECT_TRUE(pages.supportsRoutedMetadataPageRelocation(catalogReference, catalog.page_id, current));
	EXPECT_TRUE(pages.supportsRoutedMetadataPageRelocation(allocatorReference, allocator.page_id, current));
	EXPECT_TRUE(pages.supportsRoutedMetadataPageRelocation(locationReference, location.page_id, current));
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(catalogReference, MetadataPageId(99U), current)); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(routeReference, MetadataPageId(104U), current)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageFileTest, PersistsThePhysicalPageDirectoryBootstrapWithItsGeneration) {
	const Superblock current = superblock(2U, 2U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef headerReference = pageRef(1U, 1U, 2U);
	PageDirectoryBootstrap expected;
	expected.generation = Generation(2U);
	expected.committed_lsn = LogSequenceNumber(2U);
	expected.data_page_directory_root = pageRef(2U, 1U, 2U);
	expected.metadata_page_directory_root = pageRef(2U, 2U, 2U);
	pages.writePageDirectoryBootstrap(headerReference, expected);
	io->sync();
	EXPECT_EQ(pages.readPageDirectoryBootstrap(headerReference), expected);

	PageDirectoryBootstrap wrongGeneration = expected;
	wrongGeneration.generation = Generation(1U);
	expectFormatError([&] { pages.writePageDirectoryBootstrap(headerReference, wrongGeneration); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, RejectsInvalidReferencesGenerationMismatchesAndTruncatedPages) {
	const Superblock valid = superblock(1, 1);
	EXPECT_THROW(static_cast<void>(MetadataPageFile(nullptr, valid)), std::invalid_argument);
	MetadataPageFile pages(io, valid);

	expectFormatError([&] { static_cast<void>(pages.pageOffset(PhysicalPageRef{})); },
					  FormatError::FERR_INVALID_HEADER);
	PhysicalPageRef invalidFrame = pageRef(1, 1, 1);
	invalidFrame.frame_index = 0;
	expectFormatError([&] { static_cast<void>(pages.pageOffset(invalidFrame)); }, FormatError::FERR_INVALID_HEADER);
	const uint64_t overflowingSlot = std::numeric_limits<uint64_t>::max();
	expectFormatError([&] { static_cast<void>(pages.pageOffset(pageRef(overflowingSlot, 1, 1))); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(pages.readCatalogLeaf(pageRef(1, 1, 1))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	const PhysicalPageRef leafRef = pageRef(1, 1, 1);
	CatalogLeafPage leaf = leafPage(Generation(2), LogSequenceNumber(1));
	expectFormatError([&] { pages.writeCatalogLeaf(leafRef, leaf); }, FormatError::FERR_INVALID_STATE);

	leaf = leafPage(Generation(1), LogSequenceNumber(1));
	pages.writeCatalogLeaf(leafRef, leaf);
	PhysicalPageRef staleRef = leafRef;
	staleRef.generation = Generation(2);
	expectFormatError([&] { static_cast<void>(pages.readCatalogLeaf(staleRef)); }, FormatError::FERR_INVALID_STATE);

	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.level = 1;
	root.page_id = MetadataPageId(1);
	root.generation = Generation(2);
	root.committed_lsn = LogSequenceNumber(1);
	expectFormatError([&] { pages.writeCatalogRadix(leafRef, root); }, FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef rootRef = pageRef(1, 2, 1);
	root.generation = Generation(1);
	pages.writeCatalogRadix(rootRef, root);
	PhysicalPageRef staleRootRef = rootRef;
	staleRootRef.generation = Generation(2);
	expectFormatError([&] { static_cast<void>(pages.readCatalogRadix(staleRootRef)); },
					  FormatError::FERR_INVALID_STATE);

	CatalogRadixPage freeSpaceRoot = root;
	freeSpaceRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	expectFormatError([&] { pages.writeCatalogRadix(rootRef, freeSpaceRoot); }, FormatError::FERR_INVALID_STATE);
	pages.writeMetadataRadix(rootRef, freeSpaceRoot);
	EXPECT_EQ(pages.readMetadataRadix(rootRef).directory_type, MetadataDirectoryType::MDT_ALLOCATOR);
	expectFormatError([&] { static_cast<void>(pages.readCatalogRadix(rootRef)); }, FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef freeSpaceRef = pageRef(1, 3, 1);
	AllocatorLeafPage freeLeaf = freeSpaceLeaf(Generation(2), LogSequenceNumber(1));
	expectFormatError([&] { pages.writeAllocatorLeaf(freeSpaceRef, freeLeaf); }, FormatError::FERR_INVALID_STATE);
	freeLeaf = freeSpaceLeaf(Generation(1), LogSequenceNumber(1));
	pages.writeAllocatorLeaf(freeSpaceRef, freeLeaf);
	PhysicalPageRef staleFreeSpaceRef = freeSpaceRef;
	staleFreeSpaceRef.generation = Generation(2);
	expectFormatError([&] { static_cast<void>(pages.readAllocatorLeaf(staleFreeSpaceRef)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(pages.readAllocatorLeaf(freeSpaceRef).allocated_slots[0], std::byte{0b00000011});

	const PhysicalPageRef locationRef = pageRef(1, 4, 1);
	LocationLeafPage location = locationLeaf(Generation(2), LogSequenceNumber(1));
	expectFormatError([&] { pages.writeLocationLeaf(locationRef, location); }, FormatError::FERR_INVALID_STATE);
	location = locationLeaf(Generation(1), LogSequenceNumber(1));
	pages.writeLocationLeaf(locationRef, location);
	PhysicalPageRef staleLocationRef = locationRef;
	staleLocationRef.generation = Generation(2);
	expectFormatError([&] { static_cast<void>(pages.readLocationLeaf(staleLocationRef)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(pages.readLocationLeaf(locationRef).entries, location.entries);
}

TEST_F(RelocatableMetadataPageFileTest, DetectsCorruptionAndRejectsMisalignedDataRegions) {
	const PhysicalPageRef leafRef = pageRef(1, 1, 1);
	const Superblock valid = superblock(1, 1);
	MetadataPageFile pages(io, valid);
	pages.writeCatalogLeaf(leafRef, leafPage(Generation(1), LogSequenceNumber(1)));
	std::byte corrupt = std::byte{0};
	io->writeAt(pages.pageOffset(leafRef), &corrupt, sizeof(corrupt));
	io->sync();
	expectFormatError([&] { static_cast<void>(pages.readCatalogLeaf(leafRef)); }, FormatError::FERR_CHECKSUM_MISMATCH);

	Superblock badRegion = valid;
	badRegion.data_region_offset = RSS_FILE_BOOTSTRAP_BYTES + 1U;
	expectFormatError([&] { static_cast<void>(MetadataPageFile(io, badRegion)); }, FormatError::FERR_INVALID_HEADER);
}
