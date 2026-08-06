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
		page.page_id = allocator.allocatePageId();
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

	LocationLeafPage pageDirectoryLeaf(MetadataPageFile &pages, const Superblock &current) {
		const MetadataRadixPage root = pages.readMetadataRadix(current.page_directory_root);
		EXPECT_EQ(root.level, 0U);
		EXPECT_TRUE(root.children[0].isValid());
		return pages.readLocationLeaf(root.children[0]);
	}

	PhysicalPageRef pageDirectoryLeafReference(MetadataPageFile &pages, const Superblock &current) {
		return pages.readMetadataRadix(current.page_directory_root).children[0];
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
	internal.page_id = allocator.allocatePageId();
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
