/**
 * @file test_MetadataPageFile.cpp
 * @brief Durable placement and reopen tests for relocatable Catalog metadata pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <span>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
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

	class MetadataPageFileTestAllocator final : public MetadataPageAllocator {
	public:
		MetadataPageId allocateMetadataPageId() override { return MetadataPageId(nextMetadataPageId_++); }

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
													std::span<const PhysicalSlotId> reservedSlots) override {
			for (;;) {
				const uint64_t ordinal = nextPhysicalPage_++;
				const PhysicalPageRef reference{
						PhysicalSlotId(ordinal / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
						static_cast<uint8_t>(ordinal % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U), generation};
				if (std::ranges::find(reservedSlots, reference.physical_slot) == reservedSlots.end()) {
					return {reference, std::nullopt};
				}
			}
		}

		void abandonPage(PhysicalPageRef) noexcept override {}

	private:
		uint64_t nextMetadataPageId_ = 1000U;
		uint64_t nextPhysicalPage_ = 256U;
	};

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

	const CatalogLeafPage sequential = pages.readCatalogLeaf(leafRef, BufferPoolReadHint::BPRH_SEQUENTIAL);
	ASSERT_EQ(sequential.entries.size(), expected.entries.size());
	EXPECT_EQ(sequential.entries.front().segment_id, expected.entries.front().segment_id);
	EXPECT_EQ(sequential.entries.back().segment_id, expected.entries.back().segment_id);
	EXPECT_EQ(io->bufferPoolStats().hits, prefetched.hits + 1U);

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

	// Reading also authenticates the generation embedded in persisted bytes.
	const auto malformedRouteBytes = MetadataPageRouteLeafCodec::encode(wrongGeneration);
	io->writeAt(pages.pageOffset(routeReference), malformedRouteBytes.data(), malformedRouteBytes.size());
	expectFormatError([&] { static_cast<void>(pages.readMetadataPageRouteLeaf(routeReference)); },
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
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(allocatorReference, MetadataPageId(99U), current)); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(locationReference, MetadataPageId(99U), current)); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(catalogReference, MetadataPageId(99U), current)); },
			FormatError::FERR_INVALID_STATE);
	MetadataRadixPage regularRadix;
	regularRadix.kind = CatalogRadixPageKind::CRPK_ROOT;
	regularRadix.directory_type = MetadataDirectoryType::MDT_CATALOG;
	regularRadix.level = 0U;
	regularRadix.page_id = MetadataPageId(107U);
	regularRadix.generation = Generation(1U);
	regularRadix.committed_lsn = LogSequenceNumber(1U);
	const PhysicalPageRef regularRadixReference = pageRef(1U, 7U, 1U);
	pages.writeMetadataRadix(regularRadixReference, regularRadix);
	expectFormatError(
			[&] {
				static_cast<void>(
						pages.supportsRoutedMetadataPageRelocation(regularRadixReference, MetadataPageId(99U), current));
			},
			FormatError::FERR_INVALID_STATE);
	regularRadix.committed_lsn = LogSequenceNumber(2U);
	pages.writeMetadataRadix(regularRadixReference, regularRadix);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(regularRadixReference, regularRadix.page_id, current)); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(routeReference, MetadataPageId(104U), current)); },
			FormatError::FERR_INVALID_HEADER);

	// A current identity with a future publication LSN is rejected for every
	// typed routed page instead of being copied into a newer generation.
	CatalogLeafPage futureCatalog = catalog;
	futureCatalog.committed_lsn = LogSequenceNumber(2U);
	pages.writeCatalogLeaf(catalogReference, futureCatalog);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(catalogReference, catalog.page_id, current)); },
			FormatError::FERR_INVALID_STATE);
	AllocatorLeafPage futureAllocator = allocator;
	futureAllocator.committed_lsn = LogSequenceNumber(2U);
	pages.writeAllocatorLeaf(allocatorReference, futureAllocator);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(allocatorReference, allocator.page_id, current)); },
			FormatError::FERR_INVALID_STATE);
	LocationLeafPage futureLocation = location;
	futureLocation.committed_lsn = LogSequenceNumber(2U);
	pages.writeLocationLeaf(locationReference, futureLocation);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(locationReference, location.page_id, current)); },
			FormatError::FERR_INVALID_STATE);

	// A radix page used as a route target is not a relocatable metadata page.
	MetadataRadixPage routeRadix;
	routeRadix.kind = CatalogRadixPageKind::CRPK_ROOT;
	routeRadix.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
	routeRadix.level = 0U;
	routeRadix.page_id = MetadataPageId(105U);
	routeRadix.generation = Generation(1U);
	routeRadix.committed_lsn = LogSequenceNumber(1U);
	const PhysicalPageRef routeRadixReference = pageRef(1U, 5U, 1U);
	pages.writeMetadataRadix(routeRadixReference, routeRadix);
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(routeRadixReference, routeRadix.page_id, current)); },
			FormatError::FERR_INVALID_STATE);

	// An unrecognized page magic must fail closed instead of being treated as a
	// relocatable typed metadata page.
	const PhysicalPageRef unknownReference = pageRef(1U, 6U, 1U);
	std::array<std::byte, RSS_PAGE_SIZE> unknownBytes{};
	io->writeAt(pages.pageOffset(unknownReference), unknownBytes.data(), unknownBytes.size());
	expectFormatError(
			[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(unknownReference, MetadataPageId(106U), current)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageFileTest,
	   RefusesToRelocatePhysicalRadixChildrenBeforeAPageDirectoryBootstrapIsPublished) {
	MetadataPageFileTestAllocator allocator;
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef rootReference = pageRef(1U, 1U, 1U);
	const PhysicalPageRef childReference = pageRef(1U, 2U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_PAGE;
	root.level = 0U;
	root.page_id = MetadataPageId(200U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = childReference;
	pages.writeMetadataRadix(rootReference, root);

	// A physical child can be logicalized only after the published bootstrap
	// authenticates a route for every child frame. Until then relocation must
	// report it as unavailable instead of copying an unrouteable page.
	EXPECT_FALSE(pages.supportsRoutedMetadataPageRelocation(rootReference, root.page_id, current));

	Superblock rootlessBootstrap = current;
	rootlessBootstrap.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch missingBootstrapReference(allocator, rootlessBootstrap, LogSequenceNumber(2U));
	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(rootReference, root.page_id, missingBootstrapReference)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, RejectsIncompleteRoutePublicationsBeforeCopyingRadixPages) {
	MetadataPageFileTestAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	const PhysicalPageRef rootReference = pageRef(1U, 1U, 1U);
	const PhysicalPageRef childReference = pageRef(1U, 2U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_PAGE;
	root.level = 0U;
	root.page_id = MetadataPageId(200U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = childReference;
	pages.writeMetadataRadix(rootReference, root);
	const PhysicalPageRef routeRoot = MetadataPageRouteDirectory(pages).buildFromCompleteRouteSet(
			construction, std::vector<MetadataPageRoute>{{root.page_id, rootReference}});
	const PhysicalPageRef bootstrapReference = pageRef(2U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	EXPECT_FALSE(pages.supportsRoutedMetadataPageRelocation(rootReference, root.page_id, published));
	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(rootReference, root.page_id, relocation)); },
			FormatError::FERR_INVALID_STATE);

	Superblock rootless = published;
	rootless.page_directory_root = {};
	EXPECT_FALSE(pages.supportsRoutedMetadataPageRelocation(rootReference, root.page_id, rootless));
}

TEST_F(RelocatableMetadataPageFileTest, RejectsBootstrapHeadersThatDoNotMatchTheirRouteContext) {
	const Superblock current = superblock(2U, 2U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef rootReference = pageRef(1U, 1U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_PAGE;
	root.level = 0U;
	root.page_id = MetadataPageId(200U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	pages.writeMetadataRadix(rootReference, root);

	const auto rejects = [&](PhysicalPageRef bootstrapReference, PageDirectoryBootstrap bootstrap) {
		pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
		Superblock routeContext = current;
		routeContext.page_directory_root = bootstrapReference;
		routeContext.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
		expectFormatError(
				[&] { static_cast<void>(pages.supportsRoutedMetadataPageRelocation(rootReference, root.page_id, routeContext)); },
				FormatError::FERR_INVALID_HEADER);
	};

	PageDirectoryBootstrap staleGeneration;
	staleGeneration.generation = Generation(1U);
	staleGeneration.committed_lsn = LogSequenceNumber(1U);
	staleGeneration.metadata_page_directory_root = pageRef(2U, 2U, 1U);
	rejects(pageRef(2U, 1U, 1U), staleGeneration);

	PageDirectoryBootstrap staleLsn;
	staleLsn.generation = Generation(2U);
	staleLsn.committed_lsn = LogSequenceNumber(1U);
	staleLsn.metadata_page_directory_root = pageRef(2U, 4U, 2U);
	rejects(pageRef(2U, 3U, 2U), staleLsn);

	PageDirectoryBootstrap missingRoute;
	missingRoute.generation = Generation(2U);
	missingRoute.committed_lsn = LogSequenceNumber(2U);
	rejects(pageRef(2U, 5U, 2U), missingRoute);
}

TEST_F(RelocatableMetadataPageFileTest, RejectsRelocationWhoseExpectedRouteNamesAnotherPage) {
	MetadataPageFileTestAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	const PhysicalPageRef rootReference = pageRef(1U, 1U, 1U);
	const PhysicalPageRef otherReference = pageRef(1U, 2U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_PAGE;
	root.level = 0U;
	root.page_id = MetadataPageId(200U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	pages.writeMetadataRadix(rootReference, root);
	LocationLeafPage other = locationLeaf(Generation(1U), LogSequenceNumber(1U));
	other.page_id = MetadataPageId(201U);
	pages.writeLocationLeaf(otherReference, other);
	const PhysicalPageRef routeRoot = MetadataPageRouteDirectory(pages).buildFromCompleteRouteSet(
			construction, std::vector<MetadataPageRoute>{{root.page_id, rootReference}, {other.page_id, otherReference}});
	const PhysicalPageRef bootstrapReference = pageRef(2U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));

	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(rootReference, other.page_id, relocation)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, RelocatesRoutedPhysicalRadixChildrenIntoLogicalReferences) {
	MetadataPageFileTestAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	const PhysicalPageRef sourceRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef sourceLeaf = pageRef(1U, 2U, 1U);
	MetadataRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_EXTENT;
	root.level = 0U;
	root.page_id = MetadataPageId(99U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = sourceLeaf;
	pages.writeMetadataRadix(sourceRoot, root);
	LocationLeafPage leaf;
	leaf.directory_type = MetadataDirectoryType::MDT_EXTENT;
	leaf.page_id = MetadataPageId(100U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{1U, SegmentId(2U), Generation(1U), 1U, 1U}};
	pages.writeLocationLeaf(sourceLeaf, leaf);
	const PhysicalPageRef logicalRootReference = pageRef(1U, 7U, 1U);
	MetadataRadixPage logicalRoot = root;
	logicalRoot.page_id = MetadataPageId(104U);
	logicalRoot.logical_children_enabled = true;
	logicalRoot.logical_children[0] = leaf.page_id;
	logicalRoot.children[0] = {};
	pages.writeMetadataRadix(logicalRootReference, logicalRoot);
	const PhysicalPageRef catalogReference = pageRef(3U, 1U, 1U);
	const PhysicalPageRef allocatorReference = pageRef(3U, 2U, 1U);
	const PhysicalPageRef locationReference = pageRef(3U, 3U, 1U);
	CatalogLeafPage catalog = leafPage(Generation(1U), LogSequenceNumber(1U));
	catalog.page_id = MetadataPageId(101U);
	AllocatorLeafPage allocatorPage = freeSpaceLeaf(Generation(1U), LogSequenceNumber(1U));
	LocationLeafPage location = locationLeaf(Generation(1U), LogSequenceNumber(1U));
	location.page_id = MetadataPageId(103U);
	pages.writeCatalogLeaf(catalogReference, catalog);
	pages.writeAllocatorLeaf(allocatorReference, allocatorPage);
	pages.writeLocationLeaf(locationReference, location);
	const PhysicalPageRef routeRoot = MetadataPageRouteDirectory(pages).buildFromCompleteRouteSet(
			construction,
			std::vector<MetadataPageRoute>{{MetadataPageId(99U), sourceRoot}, {MetadataPageId(100U), sourceLeaf},
														{catalog.page_id, catalogReference}, {allocatorPage.page_id, allocatorReference},
														{location.page_id, locationReference},
														{logicalRoot.page_id, logicalRootReference}});
	const PhysicalPageRef bootstrapReference = pageRef(2U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_EXTENT)] = root.page_id;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	MetadataWriteBatch noBootstrap(allocator, initial, LogSequenceNumber(2U));
	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(sourceRoot, root.page_id, noBootstrap)); },
			FormatError::FERR_INVALID_STATE);

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.extent_directory_root = sourceRoot;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	EXPECT_TRUE(pages.supportsRoutedMetadataPageRelocation(sourceRoot, root.page_id, published));
	EXPECT_TRUE(pages.supportsRoutedMetadataPageRelocation(logicalRootReference, logicalRoot.page_id, published));
	const PhysicalPageRef replacement = pages.relocateRoutedMetadataPage(sourceRoot, root.page_id, relocation);
	EXPECT_NE(replacement.physical_slot, sourceRoot.physical_slot);
	const MetadataRadixPage relocated = pages.readMetadataRadixUnresolved(replacement);
	EXPECT_TRUE(relocated.logical_children_enabled);
	EXPECT_EQ(relocated.logical_children[0], leaf.page_id);
	EXPECT_FALSE(relocated.children[0].isValid());
	const MetadataRadixPage resolved = pages.readMetadataRadix(replacement, published);
	EXPECT_TRUE(resolved.logical_children_resolved);
	EXPECT_EQ(resolved.children[0], sourceLeaf);
	// Before the candidate bootstrap is written, its COW radix page resolves
	// through the immediately preceding authenticated bootstrap generation.
	const MetadataRadixPage candidateResolved = pages.readMetadataRadix(replacement, relocation.candidate());
	EXPECT_TRUE(candidateResolved.logical_children_resolved);
	EXPECT_EQ(candidateResolved.children[0], sourceLeaf);
	const PhysicalPageRef relocatedLogical =
			pages.relocateRoutedMetadataPage(logicalRootReference, logicalRoot.page_id, relocation);
	EXPECT_TRUE(pages.readMetadataRadixUnresolved(relocatedLogical).logical_children_enabled);
	const PhysicalPageRef relocatedCatalog = pages.relocateRoutedMetadataPage(catalogReference, catalog.page_id, relocation);
	const PhysicalPageRef relocatedAllocator =
			pages.relocateRoutedMetadataPage(allocatorReference, allocatorPage.page_id, relocation);
	const PhysicalPageRef relocatedLocation = pages.relocateRoutedMetadataPage(locationReference, location.page_id, relocation);
	EXPECT_EQ(pages.readCatalogLeaf(relocatedCatalog).generation, relocation.generation());
	EXPECT_EQ(pages.readAllocatorLeaf(relocatedAllocator).generation, relocation.generation());
	EXPECT_EQ(pages.readLocationLeaf(relocatedLocation).generation, relocation.generation());

	// A candidate may resolve through the immediately preceding bootstrap, but
	// an unrelated generation must fail closed instead of following stale routes.
	Superblock unrelatedContext = published;
	unrelatedContext.generation = Generation(4U);
	unrelatedContext.checkpoint_lsn = LogSequenceNumber(4U);
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(replacement, unrelatedContext)); },
				  FormatError::FERR_INVALID_STATE);

	// Exercise each bootstrap identity check through the public routed relocation
	// boundary. Write malformed bytes directly because the typed writer rejects
	// a generation mismatch before the relocation reader can inspect it.
	const auto writeBootstrapBytes = [&](const PageDirectoryBootstrap &value) {
		const auto bytes = PageDirectoryBootstrapCodec::encode(value);
		io->writeAt(pages.pageOffset(bootstrapReference), bytes.data(), bytes.size());
	};
	PageDirectoryBootstrap futureGeneration = bootstrap;
	futureGeneration.generation = Generation(1U);
	writeBootstrapBytes(futureGeneration);
	MetadataWriteBatch badGeneration(allocator, published, LogSequenceNumber(3U));
	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(sourceRoot, root.page_id, badGeneration)); },
			FormatError::FERR_INVALID_STATE);
	PageDirectoryBootstrap oldLsn = bootstrap;
	oldLsn.committed_lsn = LogSequenceNumber(1U);
	writeBootstrapBytes(oldLsn);
	MetadataWriteBatch badLsn(allocator, published, LogSequenceNumber(3U));
	expectFormatError([&] { static_cast<void>(pages.relocateRoutedMetadataPage(sourceRoot, root.page_id, badLsn)); },
				  FormatError::FERR_INVALID_STATE);
	PageDirectoryBootstrap missingRoute = bootstrap;
	missingRoute.metadata_page_directory_root = {};
	writeBootstrapBytes(missingRoute);
	MetadataWriteBatch missingRouteBatch(allocator, published, LogSequenceNumber(3U));
	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(sourceRoot, root.page_id, missingRouteBatch)); },
			FormatError::FERR_INVALID_STATE);
	writeBootstrapBytes(bootstrap);
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
	const auto malformedBootstrapBytes = PageDirectoryBootstrapCodec::encode(wrongGeneration);
	io->writeAt(pages.pageOffset(headerReference), malformedBootstrapBytes.data(), malformedBootstrapBytes.size());
	expectFormatError([&] { static_cast<void>(pages.readPageDirectoryBootstrap(headerReference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, RebindsOnlyToTheExistingDataRegionLayout) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);

	Superblock sameLayout = current;
	sameLayout.generation = Generation(2U);
	sameLayout.checkpoint_lsn = LogSequenceNumber(2U);
	EXPECT_NO_THROW(pages.rebindSuperblock(sameLayout));

	Superblock differentLayout = sameLayout;
	differentLayout.data_region_offset += RSS_SEGMENT_SIZE;
	expectFormatError([&] { pages.rebindSuperblock(differentLayout); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, RejectsWrongTypeThroughCatalogRouteContextOverload) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef reference = pageRef(1U, 1U, 1U);
	MetadataRadixPage catalogRoot;
	catalogRoot.kind = CatalogRadixPageKind::CRPK_ROOT;
	catalogRoot.directory_type = MetadataDirectoryType::MDT_CATALOG;
	catalogRoot.level = 0U;
	catalogRoot.page_id = MetadataPageId(299U);
	catalogRoot.generation = Generation(1U);
	catalogRoot.committed_lsn = LogSequenceNumber(1U);
	pages.writeCatalogRadix(reference, catalogRoot);
	EXPECT_EQ(pages.readCatalogRadix(reference, current).page_id, catalogRoot.page_id);

	MetadataRadixPage allocatorRoot;
	allocatorRoot.kind = CatalogRadixPageKind::CRPK_ROOT;
	allocatorRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	allocatorRoot.level = 0U;
	allocatorRoot.page_id = MetadataPageId(300U);
	allocatorRoot.generation = Generation(1U);
	allocatorRoot.committed_lsn = LogSequenceNumber(1U);
	pages.writeMetadataRadix(reference, allocatorRoot);
	expectFormatError([&] { static_cast<void>(pages.readCatalogRadix(reference, current)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageFileTest, ResolvesLogicalChildrenOnlyWithAnAuthenticatedRouteContext) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef rootReference = pageRef(1U, 1U, 1U);
	MetadataRadixPage logicalRoot;
	logicalRoot.kind = CatalogRadixPageKind::CRPK_ROOT;
	logicalRoot.directory_type = MetadataDirectoryType::MDT_CATALOG;
	logicalRoot.level = 0U;
	logicalRoot.page_id = MetadataPageId(201U);
	logicalRoot.generation = Generation(1U);
	logicalRoot.committed_lsn = LogSequenceNumber(1U);
	logicalRoot.logical_children_enabled = true;
	logicalRoot.logical_children[0] = MetadataPageId(1U);
	pages.writeMetadataRadix(rootReference, logicalRoot);

	// A logical page cannot be resolved without a published bootstrap route.
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(rootReference)); },
					  FormatError::FERR_INVALID_STATE);
	Superblock rootlessContext = current;
	rootlessContext.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(rootReference, rootlessContext)); },
					  FormatError::FERR_INVALID_STATE);

	Superblock differentLayout = current;
	differentLayout.data_region_offset += RSS_SEGMENT_SIZE;
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(rootReference, differentLayout)); },
					  FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef missingRouteHeader = pageRef(2U, 1U, 1U);
	PageDirectoryBootstrap missingRoute;
	missingRoute.generation = Generation(1U);
	missingRoute.committed_lsn = LogSequenceNumber(1U);
	pages.writePageDirectoryBootstrap(missingRouteHeader, missingRoute);
	Superblock missingRouteContext = current;
	missingRouteContext.page_directory_root = missingRouteHeader;
	missingRouteContext.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(rootReference, missingRouteContext)); },
					  FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef previousBootstrapReference = pageRef(2U, 2U, 1U);
	PageDirectoryBootstrap previousBootstrap;
	previousBootstrap.generation = Generation(1U);
	previousBootstrap.committed_lsn = LogSequenceNumber(1U);
	previousBootstrap.metadata_page_directory_root = pageRef(3U, 1U, 1U);
	pages.writePageDirectoryBootstrap(previousBootstrapReference, previousBootstrap);
	Superblock nonAdvancingCandidate = superblock(2U, 1U);
	nonAdvancingCandidate.page_directory_root = previousBootstrapReference;
	nonAdvancingCandidate.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	expectFormatError([&] { static_cast<void>(pages.readMetadataRadix(rootReference, nonAdvancingCandidate)); },
					  FormatError::FERR_INVALID_STATE);

	// Physical children do not require a bootstrap route and are returned as-is.
	const PhysicalPageRef physicalReference = pageRef(1U, 2U, 1U);
	MetadataRadixPage physicalRoot = logicalRoot;
	physicalRoot.page_id = MetadataPageId(202U);
	physicalRoot.logical_children_enabled = false;
	physicalRoot.logical_children[0] = {};
	pages.writeMetadataRadix(physicalReference, physicalRoot);
	EXPECT_FALSE(pages.readMetadataRadix(physicalReference).logical_children_enabled);
}

TEST_F(RelocatableMetadataPageFileTest, RejectsInvalidRoutedSourceIdentityBeforeReadingDisk) {
	const Superblock current = superblock(1U, 1U);
	MetadataPageFile pages(io, current);
	const PhysicalPageRef validSource = pageRef(1U, 1U, 1U);

	EXPECT_THROW(
			static_cast<void>(pages.supportsRoutedMetadataPageRelocation(PhysicalPageRef{}, MetadataPageId(1U), current)),
			FormatException);
	EXPECT_THROW(
			static_cast<void>(pages.supportsRoutedMetadataPageRelocation(validSource, MetadataPageId{}, current)),
			FormatException);
	PhysicalPageRef futureSource = validSource;
	futureSource.generation = Generation(2U);
	EXPECT_THROW(
			static_cast<void>(pages.supportsRoutedMetadataPageRelocation(futureSource, MetadataPageId(1U), current)),
			FormatException);
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
