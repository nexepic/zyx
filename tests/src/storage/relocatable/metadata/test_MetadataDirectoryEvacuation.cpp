/**
 * @file test_MetadataDirectoryEvacuation.cpp
 * @brief Direct fail-closed tests for metadata-directory evacuation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <functional>

#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

namespace {

	using DirectoryMutator = std::function<void(MetadataPageFile &, const Superblock &)>;
	using AllocatorDirectoryMutator =
			std::function<void(MetadataPageFile &, MetadataPagePoolAllocator &, const Superblock &)>;

	void expectDirectoryCloneFailure(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &store,
									 const DirectoryMutator &mutator, FormatError expected) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		// Construct the allocator before corrupting a source page so the test
		// exercises the evacuation validator rather than open-time recovery.
		MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
		MetadataPageFile pages(io, current);
		mutator(pages, current);
		MetadataWriteBatch batch(allocator, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		expectFormatError([&] { cloneMetadataDirectoryRootsForEvacuation(pages, batch, current); }, expected);
	}

	void expectDirectoryCloneFailureWithAllocator(const std::shared_ptr<StorageIO> &io,
															 RelocatableMetadataStore &store,
															 const AllocatorDirectoryMutator &mutator,
															 FormatError expected) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
		MetadataPageFile pages(io, current);
		mutator(pages, allocator, current);
		MetadataWriteBatch batch(allocator, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
		expectFormatError([&] { cloneMetadataDirectoryRootsForEvacuation(pages, batch, current); }, expected);
	}

	PhysicalPageRef writeAllocatorInternal(MetadataPageFile &pages, MetadataPagePoolAllocator &allocator,
														 const Superblock &current, CatalogRadixPageKind kind, uint16_t level,
														 PhysicalPageRef child) {
		const PhysicalPageRef reference = allocator.allocatePage(current.generation);
		MetadataRadixPage page;
		page.kind = kind;
		page.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
		page.level = level;
		page.page_id = allocator.allocateMetadataPageId();
		page.generation = current.generation;
		page.committed_lsn = current.checkpoint_lsn;
		page.children[0] = child;
		pages.writeMetadataRadix(reference, page);
		return reference;
	}

	void replaceAllocatorRootWithChild(MetadataPageFile &pages, const Superblock &current, uint16_t level,
														 PhysicalPageRef child) {
		MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
		root.level = level;
		root.children.fill({});
		root.children[0] = child;
		pages.writeMetadataRadix(current.allocator_root, root);
	}

	CatalogLeafPage catalogLeaf(MetadataPageFile &pages, const Superblock &current) {
		const MetadataRadixPage root = pages.readMetadataRadix(current.catalog_root);
		EXPECT_EQ(root.level, 0U);
		EXPECT_TRUE(root.children[0].isValid());
		return pages.readCatalogLeaf(root.children[0]);
	}

	PhysicalPageRef catalogLeafReference(MetadataPageFile &pages, const Superblock &current) {
		return pages.readMetadataRadix(current.catalog_root).children[0];
	}

	PhysicalPageRef pageDirectoryDataRoot(MetadataPageFile &pages, const Superblock &current) {
		if (!current.usesPageDirectoryBootstrap()) {
			return current.page_directory_root;
		}
		return pages.readPageDirectoryBootstrap(current.page_directory_root).data_page_directory_root;
	}

	LocationLeafPage pageDirectoryLeaf(MetadataPageFile &pages, const Superblock &current) {
		const MetadataRadixPage root = pages.readMetadataRadix(pageDirectoryDataRoot(pages, current));
		EXPECT_EQ(root.level, 0U);
		EXPECT_TRUE(root.children[0].isValid());
		return pages.readLocationLeaf(root.children[0]);
	}

	PhysicalPageRef pageDirectoryLeafReference(MetadataPageFile &pages, const Superblock &current) {
		return pages.readMetadataRadix(pageDirectoryDataRoot(pages, current)).children[0];
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsFutureDirectoryPage) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				root.committed_lsn = LogSequenceNumber(current.checkpoint_lsn.value() + 1U);
				pages.writeMetadataRadix(current.allocator_root, root);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationResolvesBootstrapRootsLogically) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(std::vector{entry(100, 1, 2)}, LogSequenceNumber(2)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	Superblock routed = snapshot.superblock();
	snapshot.release();
	ASSERT_TRUE(routed.usesPageDirectoryBootstrap());
	routed.catalog_root = {};
	routed.allocator_root = {};

	MetadataPagePoolAllocator allocator(io, routed, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataPageFile pages(io, routed);
	MetadataWriteBatch batch(allocator, routed, LogSequenceNumber(4));
	EXPECT_NO_THROW(cloneMetadataDirectoryRootsForEvacuation(pages, batch, routed));
	EXPECT_FALSE(batch.metadataPageRoutes().empty());
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationClonesTheBootstrapDataDirectoryAnchor) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const PageLocation location{PageId(1U), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
								RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U)};
	static_cast<void>(store->upsertLocations(std::span(&location, 1U), {}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	ASSERT_TRUE(current.usesPageDirectoryBootstrap());
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataPageFile pages(io, current);
	const PageDirectoryBootstrap before = pages.readPageDirectoryBootstrap(current.page_directory_root);
	ASSERT_TRUE(before.data_page_directory_root.isValid());

	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(4U));
	ASSERT_NO_THROW(cloneMetadataDirectoryRootsForEvacuation(pages, batch, current));
	ASSERT_TRUE(batch.candidate().page_directory_root.isValid());
	const PageDirectoryBootstrap replacement = pages.readPageDirectoryBootstrap(batch.candidate().page_directory_root);
	EXPECT_NE(replacement.data_page_directory_root, before.data_page_directory_root);
	EXPECT_EQ(pages.readMetadataRadix(replacement.data_page_directory_root).page_id,
			  pages.readMetadataRadix(before.data_page_directory_root).page_id);
}

TEST_F(RelocatableMetadataStoreTest, RoutedMetadataRelocationRejectsANoncurrentPageIdTarget) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(std::vector{entry(100U, 1U, 2U)}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPageFile pages(io, current);
	const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
	const std::vector<MetadataPageRoute> routes =
			MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(current, bootstrap.metadata_page_directory_root);
	ASSERT_GE(routes.size(), 2U);
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(4U));

	expectFormatError(
			[&] { static_cast<void>(pages.relocateRoutedMetadataPage(routes[1].physical_reference, routes[0].page_id, batch)); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(batch.metadataPageRoutes().empty());
}

TEST_F(RelocatableMetadataStoreTest, RoutedMetadataPoolRelocationRejectsAStaleBatchSuperblock) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(std::vector{entry(100U, 1U, 2U)}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPageFile pages(io, current);
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(4U));
	Superblock stale = current;
	stale.checkpoint_lsn = LogSequenceNumber(current.checkpoint_lsn.value() - 1U);

	expectFormatError(
			[&] { static_cast<void>(relocateRoutedMetadataPoolPages(pages, batch, stale, PhysicalSlotId{}, 0U)); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(batch.metadataPageRoutes().empty());
}

TEST_F(RelocatableMetadataStoreTest, RoutedMetadataPoolRelocationRejectsAnUnallocatedRouteTarget) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	std::vector<MetadataSnapshotPin> retained = createReclaimableMetadataPool(*store);
	for (MetadataSnapshotPin &snapshot: retained) {
		snapshot.release();
	}
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(7U)));
	const CatalogEntry catalog = entry(200U, 2U, 8U);
	static_cast<void>(store->upsertCatalog(std::span(&catalog, 1U), LogSequenceNumber(8U)));
	const ExtentLocation extent{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)};
	static_cast<void>(store->upsertLocations({}, std::span(&extent, 1U), LogSequenceNumber(9U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPageFile pages(io, current);
	const PageDirectoryBootstrap bootstrap = pages.readPageDirectoryBootstrap(current.page_directory_root);
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	std::optional<MetadataPageRoute> sourceRoute;
	uint32_t allocatedFrames = 0U;
	for (const MetadataPageRoute &route:
		 MetadataPageRouteDirectory(pages).collectRoutesFromRouteBranch(current, bootstrap.metadata_page_directory_root)) {
		const MetadataPagePoolHeader header = allocator.poolHeader(route.physical_reference.physical_slot);
		if (canRelocateRoutedMetadataPoolPages(pages, current, route.physical_reference.physical_slot,
																				 header.allocated_frames)) {
			sourceRoute = route;
			allocatedFrames = header.allocated_frames;
			break;
		}
	}
	ASSERT_TRUE(sourceRoute.has_value());
	const uint32_t missingRouteFrame =
			1U << (sourceRoute->physical_reference.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
	ASSERT_NE(allocatedFrames & missingRouteFrame, 0U);

	expectFormatError(
			[&] {
				static_cast<void>(canRelocateRoutedMetadataPoolPages(
						pages, current, sourceRoute->physical_reference.physical_slot, allocatedFrames & ~missingRouteFrame));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataEvacuationReportsNoCandidateForInapplicablePoolContexts) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin initialSnapshot = store->openSnapshot();
	const Superblock initial = initialSnapshot.superblock();
	initialSnapshot.release();
	MetadataPageFile initialPages(io, initial);
	const PhysicalSlotId ordinarySlot(40U);
	constexpr uint32_t oneAllocatedFrame = 1U;
	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(initialPages, initial, ordinarySlot, oneAllocatedFrame));
	EXPECT_FALSE(canEvacuateMixedRoutedMetadataPool(initialPages, initial, ordinarySlot, oneAllocatedFrame));
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(initialPages, initial, ordinarySlot, oneAllocatedFrame));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(initialPages, initial, ordinarySlot, oneAllocatedFrame));

	static_cast<void>(store->upsertCatalog(std::vector{entry(100U, 1U, 2U)}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
	MetadataSnapshotPin routedSnapshot = store->openSnapshot();
	const Superblock routed = routedSnapshot.superblock();
	routedSnapshot.release();
	ASSERT_TRUE(routed.usesPageDirectoryBootstrap());
	MetadataPageFile routedPages(io, routed);
	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(routedPages, routed, PhysicalSlotId{}, oneAllocatedFrame));
	EXPECT_FALSE(canEvacuateMixedRoutedMetadataPool(routedPages, routed, PhysicalSlotId{}, oneAllocatedFrame));
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(routedPages, routed, PhysicalSlotId{}, oneAllocatedFrame));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(routedPages, routed, PhysicalSlotId{}, oneAllocatedFrame));
	EXPECT_FALSE(canRelocateRoutedMetadataPoolPages(routedPages, routed, ordinarySlot, 0U));
	EXPECT_FALSE(canEvacuateMixedRoutedMetadataPool(routedPages, routed, ordinarySlot, 0U));
	EXPECT_FALSE(canRefreshMetadataRouteBranchForEvacuation(routedPages, routed, ordinarySlot, 0U));
	EXPECT_FALSE(canRefreshBootstrapHeaderForEvacuation(
			routedPages, routed, routed.page_directory_root.physical_slot, 0U));
}

TEST_F(RelocatableMetadataStoreTest, MetadataEvacuationRejectsAStaleBootstrapGenerationBeforeScanningRoutes) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(std::vector{entry(100U, 1U, 2U)}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock routed = snapshot.superblock();
	snapshot.release();
	ASSERT_TRUE(routed.usesPageDirectoryBootstrap());
	MetadataPageFile pages(io, routed);
	Superblock stale = routed;
	stale.generation = Generation(routed.generation.value() + 1U);
	const PhysicalSlotId sourceSlot(40U);
	constexpr uint32_t oneAllocatedFrame = 1U;
	expectFormatError(
			[&] { static_cast<void>(canRelocateRoutedMetadataPoolPages(pages, stale, sourceSlot, oneAllocatedFrame)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(canEvacuateMixedRoutedMetadataPool(pages, stale, sourceSlot, oneAllocatedFrame)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(canRefreshMetadataRouteBranchForEvacuation(
						pages, stale, sourceSlot, oneAllocatedFrame));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(canRefreshBootstrapHeaderForEvacuation(
						pages, stale, routed.page_directory_root.physical_slot, oneAllocatedFrame));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataEvacuationRejectsInvisibleOrIncompleteBootstrapHeaders) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(std::vector{entry(100U, 1U, 2U)}, LogSequenceNumber(2U)));
	static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock routed = snapshot.superblock();
	snapshot.release();
	ASSERT_TRUE(routed.usesPageDirectoryBootstrap());
	MetadataPageFile pages(io, routed);
	const PhysicalSlotId sourceSlot(40U);
	constexpr uint32_t oneAllocatedFrame = 1U;
	const auto expectRejected = [&](const PageDirectoryBootstrap &bootstrap) {
		pages.writePageDirectoryBootstrap(routed.page_directory_root, bootstrap);
		expectFormatError(
				[&] { static_cast<void>(canRelocateRoutedMetadataPoolPages(pages, routed, sourceSlot, oneAllocatedFrame)); },
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] { static_cast<void>(canEvacuateMixedRoutedMetadataPool(pages, routed, sourceSlot, oneAllocatedFrame)); },
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(canRefreshMetadataRouteBranchForEvacuation(
							pages, routed, sourceSlot, oneAllocatedFrame));
				},
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] {
					static_cast<void>(canRefreshBootstrapHeaderForEvacuation(
							pages, routed, routed.page_directory_root.physical_slot, oneAllocatedFrame));
				},
				FormatError::FERR_INVALID_HEADER);
	};

	PageDirectoryBootstrap invisible = pages.readPageDirectoryBootstrap(routed.page_directory_root);
	invisible.committed_lsn = LogSequenceNumber(routed.checkpoint_lsn.value() + 1U);
	expectRejected(invisible);

	PageDirectoryBootstrap incomplete = invisible;
	incomplete.committed_lsn = routed.checkpoint_lsn;
	incomplete.metadata_page_directory_root = {};
	expectRejected(incomplete);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsABootstrapFlagWithoutAHeader) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataPageFile pages(io, current);
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2U));
	Superblock missingHeader = current;
	missingHeader.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	missingHeader.page_directory_root = {};
	expectFormatError([&] { cloneMetadataDirectoryRootsForEvacuation(pages, batch, missingHeader); },
				  FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(batch.metadataPageRoutes().empty());
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsRootWithInternalKind) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				root.kind = CatalogRadixPageKind::CRPK_INTERNAL;
				pages.writeMetadataRadix(current.allocator_root, root);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsRepeatedSourceFrame) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				root.children[1] = root.children[0];
				pages.writeMetadataRadix(current.allocator_root, root);
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsRepeatedLogicalPageId) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				const MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				AllocatorLeafPage leaf = pages.readAllocatorLeaf(root.children[0]);
				leaf.page_id = root.page_id;
				pages.writeAllocatorLeaf(root.children[0], leaf);
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsMisroutedAllocatorLeaf) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				const MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				AllocatorLeafPage leaf = pages.readAllocatorLeaf(root.children[0]);
				leaf.first_slot = PhysicalSlotId(2U);
				pages.writeAllocatorLeaf(root.children[0], leaf);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsEmptyCatalogLeaf) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(std::vector{entry(40U, 1U, 2U)}, LogSequenceNumber(2U)));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				CatalogLeafPage leaf = catalogLeaf(pages, current);
				leaf.entries.clear();
				leaf.minimum_segment_id = {};
				leaf.maximum_segment_id = {};
				pages.writeCatalogLeaf(catalogLeafReference(pages, current), leaf);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsMisroutedCatalogLeaf) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(std::vector{entry(40U, 1U, 2U)}, LogSequenceNumber(2U)));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				CatalogLeafPage leaf = catalogLeaf(pages, current);
				ASSERT_FALSE(leaf.entries.empty());
				for (size_t index = 0U; index < leaf.entries.size(); ++index) {
					leaf.entries[index].segment_id = SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U + index);
				}
				leaf.minimum_segment_id = leaf.entries.front().segment_id;
				leaf.maximum_segment_id = leaf.entries.back().segment_id;
				pages.writeCatalogLeaf(catalogLeafReference(pages, current), leaf);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsWrongLocationLeafType) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const PageLocation location{PageId(1U), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
									RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U)};
	static_cast<void>(store->upsertLocations(std::span(&location, 1U), {}, LogSequenceNumber(2U)));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				LocationLeafPage leaf = pageDirectoryLeaf(pages, current);
				leaf.directory_type = MetadataDirectoryType::MDT_EXTENT;
				pages.writeLocationLeaf(pageDirectoryLeafReference(pages, current), leaf);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsMisroutedLocationLeaf) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const PageLocation location{PageId(1U), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
									RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U)};
	static_cast<void>(store->upsertLocations(std::span(&location, 1U), {}, LogSequenceNumber(2U)));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				LocationLeafPage leaf = pageDirectoryLeaf(pages, current);
				ASSERT_EQ(leaf.entries.size(), 1U);
				leaf.entries.front().logical_id = RSS_LOCATION_LEAF_MAX_ENTRIES + 1U;
				pages.writeLocationLeaf(pageDirectoryLeafReference(pages, current), leaf);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationClonesNestedRadixDirectory) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	MetadataPageFile pages(io, current);
	MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
	ASSERT_TRUE(root.children[0].isValid());
	const PhysicalPageRef internalReference = allocator.allocatePage(current.generation);
	MetadataRadixPage internal;
	internal.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	internal.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	internal.level = 0U;
	internal.page_id = allocator.allocateMetadataPageId();
	internal.generation = current.generation;
	internal.committed_lsn = current.checkpoint_lsn;
	internal.children[0] = root.children[0];
	pages.writeMetadataRadix(internalReference, internal);
	root.level = 1U;
	root.children.fill({});
	root.children[0] = internalReference;
	pages.writeMetadataRadix(current.allocator_root, root);

	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));
	EXPECT_NO_THROW(cloneMetadataDirectoryRootsForEvacuation(pages, batch, current));
	EXPECT_TRUE(batch.candidate().allocator_root.isValid());
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsNestedDirectoryWithWrongKind) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailureWithAllocator(
			io, *store,
			[](MetadataPageFile &pages, MetadataPagePoolAllocator &allocator, const Superblock &current) {
				const MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				const PhysicalPageRef child = writeAllocatorInternal(
						pages, allocator, current, CatalogRadixPageKind::CRPK_ROOT, 0U, root.children[0]);
				replaceAllocatorRootWithChild(pages, current, 1U, child);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsNestedDirectoryWithWrongLevel) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailureWithAllocator(
			io, *store,
			[](MetadataPageFile &pages, MetadataPagePoolAllocator &allocator, const Superblock &current) {
				const MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				const PhysicalPageRef child = writeAllocatorInternal(
						pages, allocator, current, CatalogRadixPageKind::CRPK_INTERNAL, 1U, root.children[0]);
				replaceAllocatorRootWithChild(pages, current, 1U, child);
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsAllocatorLeafRouteThatOverflowsSlotMath) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailureWithAllocator(
			io, *store,
			[](MetadataPageFile &pages, MetadataPagePoolAllocator &allocator, const Superblock &current) {
				MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				PhysicalPageRef child = root.children[0];
				for (uint16_t level = 0U; level < RSS_CATALOG_RADIX_MAX_LEVEL; ++level) {
					child = writeAllocatorInternal(pages, allocator, current, CatalogRadixPageKind::CRPK_INTERNAL,
																	 level, child);
				}
				root.level = RSS_CATALOG_RADIX_MAX_LEVEL;
				root.children.fill({});
				root.children[1] = child;
				pages.writeMetadataRadix(current.allocator_root, root);
			},
		FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataDirectoryEvacuationRejectsOverflowingChildOrdinal) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectDirectoryCloneFailure(
			io, *store,
			[](MetadataPageFile &pages, const Superblock &current) {
				MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
				ASSERT_TRUE(root.children[0].isValid());
				const PhysicalPageRef source = root.children[0];
				root.level = RSS_CATALOG_RADIX_MAX_LEVEL;
				root.children.fill({});
				root.children[2] = source;
				pages.writeMetadataRadix(current.allocator_root, root);
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
}
