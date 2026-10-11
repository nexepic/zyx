/**
 * @file test_MetadataPageRouteDirectory.cpp
 * @brief Physical bootstrap route tests for typed metadata page IDs.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
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

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		return {PhysicalSlotId(slot), frame, Generation(generation)};
	}

	class TestMetadataPageAllocator final : public MetadataPageAllocator {
	public:
		MetadataPageId allocateMetadataPageId() override { return MetadataPageId(nextMetadataPageId++); }

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
											std::span<const PhysicalSlotId> reservedSlots) override {
			for (;;) {
				const uint64_t index = nextPhysicalPage++;
				const PhysicalPageRef reference{PhysicalSlotId(index / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
													 static_cast<uint8_t>(index % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U), generation};
				if (std::ranges::find(reservedSlots, reference.physical_slot) == reservedSlots.end()) {
					return {reference, std::nullopt};
				}
			}
		}

		void abandonPage(PhysicalPageRef) noexcept override {}

	private:
		uint64_t nextMetadataPageId = 1U;
		uint64_t nextPhysicalPage = 128U;
	};

	MetadataPageRoute route(uint64_t metadataPageId, uint64_t slot, uint8_t frame, uint64_t generation) {
		return {MetadataPageId(metadataPageId), pageRef(slot, frame, generation)};
	}

} // namespace

class RelocatableMetadataPageRouteDirectoryTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
				("relocatable_metadata_page_route_directory_" + to_string(uuid) + ".dat");
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

TEST_F(RelocatableMetadataPageRouteDirectoryTest, ResolvesCompleteRoutesAcrossMultipleRadixLevelsAfterReopen) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const uint64_t farLeafOrdinal =
			static_cast<uint64_t>(RSS_CATALOG_RADIX_FANOUT) * RSS_CATALOG_RADIX_FANOUT;
	const std::vector<MetadataPageRoute> routes = {
			route(1U, 1000U, 1U, 1U),
			route(farLeafOrdinal * RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U, 1001U, 1U, 1U),
	};

	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, routes);
	EXPECT_TRUE(root.isValid());
	EXPECT_TRUE(batch.metadataPageRoutes().empty());
	EXPECT_EQ(pages.readMetadataRadix(root).level, 3U);
	EXPECT_EQ(directory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	EXPECT_EQ(directory.find(batch.candidate(), root, routes[1].page_id), routes[1].physical_reference);
	io->sync();

	reopen();
	MetadataPageFile reopenedPages(io, batch.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(batch.candidate(), root, MetadataPageId(1U)), routes[0].physical_reference);
	EXPECT_EQ(reopenedDirectory.find(batch.candidate(), root, routes[1].page_id), routes[1].physical_reference);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsMissingFutureAndWronglyTypedRoutes) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const std::vector<MetadataPageRoute> routes = {route(1U, 1000U, 1U, 1U)};
	const PhysicalPageRef root = directory.buildFromCompleteRouteSet(batch, routes);

	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(2U))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.find(current, root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);

	MetadataRadixPage wrongRoot = pages.readMetadataRadix(root);
	wrongRoot.directory_type = MetadataDirectoryType::MDT_PAGE;
	pages.writeMetadataRadix(root, wrongRoot);
	expectFormatError([&] { static_cast<void>(directory.find(batch.candidate(), root, MetadataPageId(1U))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, ResolvesLogicalRadixChildrenThroughPublishedBootstrapRoutes) {
	const Superblock current = superblock(1U, 1U);
	const PhysicalPageRef parentReference = pageRef(41U, 1U, 1U);
	const PhysicalPageRef childReference = pageRef(42U, 1U, 1U);
	const PhysicalPageRef bootstrapReference = pageRef(43U, 1U, 2U);
	MetadataPageFile pages(io, current);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(batch, std::vector{route(900U, childReference.physical_slot.value(),
																				 childReference.frame_index, 1U)});

	MetadataRadixPage parent;
	parent.kind = CatalogRadixPageKind::CRPK_ROOT;
	parent.directory_type = MetadataDirectoryType::MDT_CATALOG;
	parent.level = 0U;
	parent.page_id = MetadataPageId(901U);
	parent.generation = Generation(1U);
	parent.committed_lsn = LogSequenceNumber(1U);
	parent.logical_children_enabled = true;
	parent.logical_children[0] = MetadataPageId(900U);
	pages.writeMetadataRadix(parentReference, parent);

	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataPageFile publishedPages(io, published);
	const MetadataRadixPage resolved = publishedPages.readMetadataRadix(parentReference);
	EXPECT_TRUE(resolved.logical_children_enabled);
	EXPECT_EQ(resolved.logical_children[0], MetadataPageId(900U));
	EXPECT_EQ(resolved.children[0], childReference);

	Superblock candidate = published;
	candidate.generation = Generation(3U);
	candidate.checkpoint_lsn = LogSequenceNumber(3U);
	EXPECT_EQ(publishedPages.readMetadataRadix(parentReference, candidate).children[0], childReference);
	candidate.generation = Generation(4U);
	candidate.checkpoint_lsn = LogSequenceNumber(4U);
	expectFormatError([&] { static_cast<void>(publishedPages.readMetadataRadix(parentReference, candidate)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RejectsUnorderedIncompleteOrFutureRouteSets) {
	const Superblock current = superblock(1U, 1U);
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());
	MetadataPageRouteDirectory directory(pages);

	expectFormatError([&] { static_cast<void>(directory.buildFromCompleteRouteSet(batch, {})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(
						batch, std::vector{route(2U, 1001U, 1U, 1U), route(1U, 1000U, 1U, 1U)}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(directory.buildFromCompleteRouteSet(batch, std::vector{route(1U, 1000U, 1U, 3U)}));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CountsAllFramesBeforeInitialBootstrapAllocation) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	MetadataPageFile pages(io, current);
	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(101U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage leaf;
	leaf.page_id = MetadataPageId(102U);
	leaf.minimum_segment_id = SegmentId(1U);
	leaf.maximum_segment_id = SegmentId(1U);
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	leaf.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
						 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, leaf);
	MetadataPageRouteDirectory directory(pages);
	EXPECT_EQ(directory.initialBootstrapFrameCount(current), 3U);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CollectsEveryVisiblePhysicalDirectoryPageBeforeBootstrapMigration) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	const PhysicalPageRef pageRoot = pageRef(1U, 3U, 1U);
	const PhysicalPageRef pageLeaf = pageRef(1U, 4U, 1U);
	current.catalog_root = catalogRoot;
	current.page_directory_root = pageRoot;
	MetadataPageFile pages(io, current);

	CatalogRadixPage catalogRadix;
	catalogRadix.kind = CatalogRadixPageKind::CRPK_ROOT;
	catalogRadix.directory_type = MetadataDirectoryType::MDT_CATALOG;
	catalogRadix.level = 0U;
	catalogRadix.page_id = MetadataPageId(101U);
	catalogRadix.generation = Generation(1U);
	catalogRadix.committed_lsn = LogSequenceNumber(1U);
	catalogRadix.children[0] = catalogLeaf;
	CatalogLeafPage catalog;
	catalog.page_id = MetadataPageId(102U);
	catalog.minimum_segment_id = SegmentId(1U);
	catalog.maximum_segment_id = SegmentId(1U);
	catalog.generation = Generation(1U);
	catalog.committed_lsn = LogSequenceNumber(1U);
	catalog.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
							Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, catalogRadix);
	pages.writeCatalogLeaf(catalogLeaf, catalog);

	CatalogRadixPage locationRadix = catalogRadix;
	locationRadix.directory_type = MetadataDirectoryType::MDT_PAGE;
	locationRadix.page_id = MetadataPageId(201U);
	locationRadix.children.fill({});
	locationRadix.children[0] = pageLeaf;
	LocationLeafPage location;
	location.directory_type = MetadataDirectoryType::MDT_PAGE;
	location.page_id = MetadataPageId(202U);
	location.generation = Generation(1U);
	location.committed_lsn = LogSequenceNumber(1U);
	location.entries = {{1U, SegmentId(22U), Generation(1U), 1U, 1U}};
	pages.writeMetadataRadix(pageRoot, locationRadix);
	pages.writeLocationLeaf(pageLeaf, location);

	MetadataPageRouteDirectory directory(pages);
	EXPECT_EQ(directory.collectRoutesFromPhysicalDirectoryRoots(current),
			  (std::vector<MetadataPageRoute>{{MetadataPageId(101U), catalogRoot},
													 {MetadataPageId(102U), catalogLeaf},
													 {MetadataPageId(201U), pageRoot},
													 {MetadataPageId(202U), pageLeaf}}));

	location.page_id = MetadataPageId(101U);
	pages.writeLocationLeaf(pageLeaf, location);
	expectFormatError([&] { static_cast<void>(directory.collectRoutesFromPhysicalDirectoryRoots(current)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, StagedBootstrapOverlaysFinalBatchFramesWithoutDroppingCurrentRoutes) {
	Superblock current = superblock(1U, 1U);
	const PhysicalPageRef catalogRoot = pageRef(1U, 1U, 1U);
	const PhysicalPageRef catalogLeaf = pageRef(1U, 2U, 1U);
	current.catalog_root = catalogRoot;
	TestMetadataPageAllocator allocator;
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	MetadataPageFile pages(io, batch.candidate());

	CatalogRadixPage root;
	root.kind = CatalogRadixPageKind::CRPK_ROOT;
	root.directory_type = MetadataDirectoryType::MDT_CATALOG;
	root.level = 0U;
	root.page_id = MetadataPageId(101U);
	root.generation = Generation(1U);
	root.committed_lsn = LogSequenceNumber(1U);
	root.children[0] = catalogLeaf;
	CatalogLeafPage original;
	original.page_id = MetadataPageId(102U);
	original.minimum_segment_id = SegmentId(1U);
	original.maximum_segment_id = SegmentId(1U);
	original.generation = Generation(1U);
	original.committed_lsn = LogSequenceNumber(1U);
	original.entries = {{SegmentId(1U), PhysicalSlotId(100U), SegmentKind::SGK_ENTITY_PAGE_POOL, 7U,
								 Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)}};
	pages.writeCatalogRadix(catalogRoot, root);
	pages.writeCatalogLeaf(catalogLeaf, original);

	CatalogLeafPage replacement = original;
	replacement.generation = batch.generation();
	replacement.committed_lsn = batch.checkpointLsn();
	const PhysicalPageRef replacementRef = batch.allocatePage();
	pages.writeCatalogLeaf(replacementRef, replacement);
	batch.registerMetadataPageRoute(replacement.page_id, replacementRef);

	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef bootstrapReference = directory.prepareInitialBootstrapMigration(batch);
	EXPECT_EQ(batch.candidate().page_directory_root, bootstrapReference);
	EXPECT_TRUE(batch.candidate().usesPageDirectoryBootstrap());
	const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(bootstrapReference);
	EXPECT_FALSE(bootstrap.data_page_directory_root.isValid());
	EXPECT_EQ(bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_CATALOG)],
			  MetadataPageId(101U));
	EXPECT_FALSE(bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_ALLOCATOR)].isValid());
	EXPECT_EQ(directory.find(batch.candidate(), bootstrap.metadata_page_directory_root, MetadataPageId(101U)),
			  catalogRoot);
	EXPECT_EQ(directory.find(batch.candidate(), bootstrap.metadata_page_directory_root, MetadataPageId(102U)),
			  replacementRef);
	EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), bootstrap.metadata_page_directory_root),
			  (std::vector<MetadataPageRoute>{{MetadataPageId(101U), catalogRoot},
													 {MetadataPageId(102U), replacementRef}}));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, NextRouteBranchOverlaysPublishedRoutesWithCurrentBatchFrames) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	PhysicalPageRef publishedRoot;
	{
		MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
		MetadataPageFile pages(io, firstBatch.candidate());
		MetadataPageRouteDirectory directory(pages);
		publishedRoot = directory.buildFromCompleteRouteSet(
				firstBatch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	}

	const Superblock published = superblock(2U, 2U);
	MetadataWriteBatch nextBatch(allocator, published, LogSequenceNumber(3U));
	MetadataPageFile pages(io, nextBatch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef replacement = nextBatch.allocatePage();
	nextBatch.registerMetadataPageRoute(MetadataPageId(2U), replacement);
	const PhysicalPageRef nextRoot = directory.buildNextRouteBranch(nextBatch, publishedRoot);
	EXPECT_EQ(directory.find(nextBatch.candidate(), nextRoot, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	EXPECT_EQ(directory.find(nextBatch.candidate(), nextRoot, MetadataPageId(2U)), replacement);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, CountsExactFramesForNextBootstrapRoutePublication) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	PhysicalPageRef bootstrapReference;
	{
		MetadataWriteBatch firstBatch(allocator, initial, LogSequenceNumber(2U));
		MetadataPageFile pages(io, firstBatch.candidate());
		MetadataPageRouteDirectory directory(pages);
		const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
				firstBatch, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = firstBatch.generation();
		bootstrap.committed_lsn = firstBatch.checkpointLsn();
		bootstrap.metadata_page_directory_root = routeRoot;
		bootstrapReference = firstBatch.allocatePage();
		pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	}

	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	MetadataWriteBatch nextBatch(allocator, published, LogSequenceNumber(3U));
	MetadataPageFile pages(io, nextBatch.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef replacement = nextBatch.allocatePage();
	nextBatch.registerMetadataPageRoute(MetadataPageId(2U), replacement);
	EXPECT_EQ(directory.nextBootstrapFrameCount(nextBatch), 3U);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   IdentifiesACompleteRouteBranchPoolForRefreshOnlyEvacuation) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			construction, std::vector{route(1U, 1000U, 1U, 1U), route(2U, 1001U, 1U, 1U)});
	const std::vector<PhysicalPageRef> branchFrames =
			directory.collectRouteBranchFrames(construction.candidate(), routeRoot);
	ASSERT_FALSE(branchFrames.empty());
	const PhysicalSlotId sourceSlot = branchFrames.front().physical_slot;
	uint32_t sourceAllocatedFrames = 0U;
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_EQ(frame.physical_slot, sourceSlot);
		sourceAllocatedFrames |= 1U << (frame.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	}

	const PhysicalPageRef bootstrapReference = pageRef(sourceSlot.value() + 1U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	EXPECT_TRUE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceAllocatedFrames));
	MetadataWriteBatch refresh(allocator, published, LogSequenceNumber(3U));
	const std::optional<uint32_t> refreshed =
			refreshMetadataRouteBranchForEvacuation(pages, refresh, published, sourceSlot, sourceAllocatedFrames);
	ASSERT_TRUE(refreshed.has_value());
	EXPECT_EQ(*refreshed, branchFrames.size());
	EXPECT_TRUE(refresh.bootstrapRouteRefreshRequested());
	EXPECT_TRUE(refresh.metadataPageRoutes().empty());
	EXPECT_THROW(static_cast<void>(canRefreshMetadataRouteBranchForEvacuation(
					 pages, published, sourceSlot,
					 sourceAllocatedFrames & ~(1U << (branchFrames.front().frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)))),
			 FormatException);

	bootstrap.data_page_directory_root = branchFrames.front();
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceAllocatedFrames));
	bootstrap.data_page_directory_root = {};
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);

	MetadataWriteBatch refreshAndPublish(allocator, published, LogSequenceNumber(3U));
	refreshAndPublish.reserveDataSlot(sourceSlot);
	ASSERT_TRUE(refreshMetadataRouteBranchForEvacuation(pages, refreshAndPublish, published, sourceSlot,
																	 sourceAllocatedFrames)
					.has_value());
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(refreshAndPublish, {});
	EXPECT_NE(replacementHeader.physical_slot, sourceSlot);
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	EXPECT_NE(replacement.metadata_page_directory_root.physical_slot, sourceSlot);
	// A pinned old Superblock still owns its original physical branch until the
	// metadata-pool reclaimer observes that the snapshot has been released.
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(1U)), pageRef(1000U, 1U, 1U));
	EXPECT_EQ(directory.find(refreshAndPublish.candidate(), replacement.metadata_page_directory_root, MetadataPageId(1U)),
			  pageRef(1000U, 1U, 1U));
	io->sync();
	reopen();
	MetadataPageFile reopenedPages(io, refreshAndPublish.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(refreshAndPublish.candidate(), replacement.metadata_page_directory_root,
																	 MetadataPageId(2U)),
			  pageRef(1001U, 1U, 1U));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   EvacuatesMixedRoutedTargetsRouteBranchAndBootstrapHeaderFromOnePool) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeTarget = pageRef(5U, RSS_PAGE_POOL_DATA_FRAME_COUNT, 1U);
	MetadataRadixPage targetPage;
	targetPage.kind = CatalogRadixPageKind::CRPK_ROOT;
	targetPage.directory_type = MetadataDirectoryType::MDT_PAGE;
	targetPage.level = 0U;
	targetPage.page_id = MetadataPageId(99U);
	targetPage.generation = Generation(1U);
	targetPage.committed_lsn = LogSequenceNumber(1U);
	targetPage.logical_children_enabled = true;
	pages.writeMetadataRadix(routeTarget, targetPage);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 5U, routeTarget.frame_index, 1U)});
	const std::vector<PhysicalPageRef> branchFrames =
			directory.collectRouteBranchFrames(construction.candidate(), routeRoot);
	ASSERT_FALSE(branchFrames.empty());
	const PhysicalSlotId sourceSlot = branchFrames.front().physical_slot;
	ASSERT_EQ(sourceSlot, routeTarget.physical_slot);
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_EQ(frame.physical_slot, sourceSlot);
		ASSERT_NE(frame.frame_index, routeTarget.frame_index);
	}
	const PhysicalPageRef sourceHeader = pageRef(sourceSlot.value(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U, 2U);
	for (const PhysicalPageRef frame: branchFrames) {
		ASSERT_NE(frame.frame_index, sourceHeader.frame_index);
	}
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.data_page_directory_root = routeTarget;
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(sourceHeader, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = sourceHeader;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	uint32_t sourceFrames = 0U;
	for (const PhysicalPageRef frame: branchFrames) {
		sourceFrames |= 1U << (frame.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	}
	sourceFrames |= 1U << (routeTarget.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	sourceFrames |= 1U << (sourceHeader.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	ASSERT_TRUE(canEvacuateMixedRoutedMetadataPool(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceSlot, sourceFrames));
	EXPECT_FALSE(canEvacuateMixedRoutedMetadataPool(
			pages, published, sourceSlot, sourceFrames | (1U << (RSS_PAGE_POOL_DATA_FRAME_COUNT - 3U))));
	Superblock stale = published;
	stale.checkpoint_lsn = LogSequenceNumber(1U);
	MetadataWriteBatch staleBatch(allocator, published, LogSequenceNumber(3U));
	expectFormatError(
			[&] { static_cast<void>(evacuateMixedRoutedMetadataPool(pages, staleBatch, stale, sourceSlot, sourceFrames)); },
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(sourceSlot);
	ASSERT_EQ(evacuateMixedRoutedMetadataPool(pages, relocation, published, sourceSlot, sourceFrames),
			  std::popcount(sourceFrames));
	ASSERT_TRUE(relocation.bootstrapRouteRefreshRequested());
	ASSERT_TRUE(relocation.pendingBootstrapDataRoot().has_value());
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(relocation, *relocation.pendingBootstrapDataRoot());
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	const PhysicalPageRef replacementTarget =
			directory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U));
	EXPECT_NE(replacementHeader.physical_slot, sourceSlot);
	EXPECT_NE(replacement.metadata_page_directory_root.physical_slot, sourceSlot);
	EXPECT_NE(replacementTarget, routeTarget);
	EXPECT_NE(replacementTarget.physical_slot, sourceSlot);
	EXPECT_EQ(replacement.data_page_directory_root, replacementTarget);
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), routeTarget);
	EXPECT_EQ(pages.readMetadataRadix(routeTarget, published).page_id, MetadataPageId(99U));
	EXPECT_EQ(pages.readMetadataRadix(replacementTarget, relocation.candidate()).page_id, MetadataPageId(99U));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RelocatesThePhysicalDataDirectoryAnchorWithItsRoute) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef dataRoot = pageRef(9U, 1U, 1U);
	MetadataRadixPage dataPage;
	dataPage.kind = CatalogRadixPageKind::CRPK_ROOT;
	dataPage.directory_type = MetadataDirectoryType::MDT_PAGE;
	dataPage.level = 0U;
	dataPage.page_id = MetadataPageId(99U);
	dataPage.generation = Generation(1U);
	dataPage.committed_lsn = LogSequenceNumber(1U);
	dataPage.logical_children_enabled = true;
	pages.writeMetadataRadix(dataRoot, dataPage);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 9U, 1U, 1U)});
	const PhysicalPageRef bootstrapReference = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.data_page_directory_root = dataRoot;
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(dataRoot.physical_slot);
	const uint32_t dataRootFrame = 1U << (dataRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(relocateRoutedMetadataPoolPages(pages, relocation, published, dataRoot.physical_slot, dataRootFrame), 1U);
	ASSERT_TRUE(relocation.pendingBootstrapDataRoot().has_value());
	const PhysicalPageRef replacementDataRoot = *relocation.pendingBootstrapDataRoot();
	EXPECT_NE(replacementDataRoot.physical_slot, dataRoot.physical_slot);
	static_cast<void>(directory.publishNextBootstrap(relocation, replacementDataRoot));
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(relocation.candidate().page_directory_root);
	EXPECT_EQ(replacement.data_page_directory_root, replacementDataRoot);
	EXPECT_EQ(directory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  replacementDataRoot);
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), dataRoot);
	io->sync();
	reopen();
	MetadataPageFile reopenedPages(io, relocation.candidate());
	MetadataPageRouteDirectory reopenedDirectory(reopenedPages);
	EXPECT_EQ(reopenedDirectory.find(relocation.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  replacementDataRoot);
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest,
	   RelocatesARoutedPhysicalChildPageByPublishingLogicalChildren) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef sourceRoot = pageRef(9U, 1U, 1U);
	const PhysicalPageRef sourceLeaf = pageRef(9U, 2U, 1U);
	const PhysicalPageRef unrouteableRoot = pageRef(11U, 1U, 1U);
	const PhysicalPageRef unrouteableChild = pageRef(12U, 1U, 1U);
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
	MetadataRadixPage physicalChildWithoutRoute = root;
	physicalChildWithoutRoute.page_id = MetadataPageId(101U);
	physicalChildWithoutRoute.children[0] = unrouteableChild;
	pages.writeMetadataRadix(unrouteableRoot, physicalChildWithoutRoute);
	const PhysicalPageRef routeRoot = directory.buildFromCompleteRouteSet(
			construction,
			std::vector{route(99U, 9U, 1U, 1U), route(100U, 9U, 2U, 1U), route(101U, 11U, 1U, 1U)});
	const PhysicalPageRef bootstrapReference = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	bootstrap.metadata_directory_roots[static_cast<size_t>(MetadataDirectoryRootOrdinal::MDRO_EXTENT)] =
			MetadataPageId(99U);
	pages.writePageDirectoryBootstrap(bootstrapReference, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = bootstrapReference;
	published.extent_directory_root = sourceRoot;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	const uint32_t sourceFrames = (1U << (sourceRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)) |
								  (1U << (sourceLeaf.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME));
	ASSERT_TRUE(canRelocateRoutedMetadataPoolPages(pages, published, sourceRoot.physical_slot, sourceFrames));

	MetadataWriteBatch relocation(allocator, published, LogSequenceNumber(3U));
	relocation.reserveDataSlot(sourceRoot.physical_slot);
	ASSERT_EQ(relocateRoutedMetadataPoolPages(pages, relocation, published, sourceRoot.physical_slot, sourceFrames), 2U);
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(relocation, {});
	const PageDirectoryBootstrap replacementBootstrap = pages.readPageDirectoryBootstrap(replacementHeader);
	const PhysicalPageRef replacementRoot = directory.find(
			relocation.candidate(), replacementBootstrap.metadata_page_directory_root, MetadataPageId(99U));
	const PhysicalPageRef replacementLeaf = directory.find(
			relocation.candidate(), replacementBootstrap.metadata_page_directory_root, MetadataPageId(100U));
	EXPECT_NE(replacementRoot.physical_slot, sourceRoot.physical_slot);
	EXPECT_NE(replacementLeaf.physical_slot, sourceLeaf.physical_slot);
	EXPECT_EQ(relocation.candidate().extent_directory_root, replacementRoot);
	EXPECT_FALSE(pages.readMetadataRadixUnresolved(sourceRoot).logical_children_enabled);
	const MetadataRadixPage replacementRootPage = pages.readMetadataRadix(replacementRoot, relocation.candidate());
	EXPECT_TRUE(replacementRootPage.logical_children_enabled);
	EXPECT_EQ(replacementRootPage.logical_children[0], MetadataPageId(100U));
	EXPECT_EQ(replacementRootPage.children[0], replacementLeaf);
	EXPECT_EQ(pages.readMetadataRadix(sourceRoot, published).children[0], sourceLeaf);

	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(
			pages, published, unrouteableRoot.physical_slot,
			1U << (unrouteableRoot.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME)));
}

TEST_F(RelocatableMetadataPageRouteDirectoryTest, RefreshesAnIsolatedBootstrapHeaderPoolDuringEvacuation) {
	TestMetadataPageAllocator allocator;
	const Superblock initial = superblock(1U, 1U);
	MetadataWriteBatch construction(allocator, initial, LogSequenceNumber(2U));
	MetadataPageFile pages(io, construction.candidate());
	MetadataPageRouteDirectory directory(pages);
	const PhysicalPageRef routeRoot =
			directory.buildFromCompleteRouteSet(construction, std::vector{route(99U, 9U, 1U, 1U)});
	const PhysicalPageRef sourceHeader = pageRef(10U, 1U, 2U);
	PageDirectoryBootstrap bootstrap;
	bootstrap.generation = Generation(2U);
	bootstrap.committed_lsn = LogSequenceNumber(2U);
	bootstrap.metadata_page_directory_root = routeRoot;
	pages.writePageDirectoryBootstrap(sourceHeader, bootstrap);
	Superblock published = superblock(2U, 2U);
	published.page_directory_root = sourceHeader;
	published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	const uint32_t sourceFrame = 1U << (sourceHeader.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	ASSERT_TRUE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceHeader.physical_slot, sourceFrame));

	MetadataWriteBatch refresh(allocator, published, LogSequenceNumber(3U));
	refresh.reserveDataSlot(sourceHeader.physical_slot);
	ASSERT_EQ(refreshBootstrapHeaderForEvacuation(pages, refresh, published, sourceHeader.physical_slot, sourceFrame),
			  1U);
	ASSERT_TRUE(refresh.bootstrapRouteRefreshRequested());
	const PhysicalPageRef replacementHeader = directory.publishNextBootstrap(refresh, {});
	EXPECT_NE(replacementHeader.physical_slot, sourceHeader.physical_slot);
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(replacementHeader);
	EXPECT_EQ(directory.find(refresh.candidate(), replacement.metadata_page_directory_root, MetadataPageId(99U)),
			  pageRef(9U, 1U, 1U));
	EXPECT_EQ(directory.find(published, routeRoot, MetadataPageId(99U)), pageRef(9U, 1U, 1U));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(pages, published, sourceHeader.physical_slot,
																	 sourceFrame | (1U << RSS_PAGE_POOL_FIRST_DATA_FRAME)));
}
