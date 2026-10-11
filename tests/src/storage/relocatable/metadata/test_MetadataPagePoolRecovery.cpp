/**
 * @file test_MetadataPagePoolRecovery.cpp
 * @brief Recovery, cleanup, and root-discovery tests for metadata page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
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
		result.owner_id = 19U;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	MetadataRadixPage radixPage(MetadataPageId pageId, CatalogRadixPageKind kind, uint16_t level, Generation generation,
									   LogSequenceNumber lsn) {
		MetadataRadixPage page;
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
			case MetadataDirectoryType::MDT_CATALOG:
			case MetadataDirectoryType::MDT_ALLOCATOR:
			case MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE:
				break;
		}
		throw std::invalid_argument("test helper requires a Location Directory type");
	}

	PhysicalPageRef writeLocationDirectoryRoot(MetadataPageFile &pages, MetadataPagePoolAllocator &allocator,
														 MetadataDirectoryType directoryType, Superblock &superblock) {
		const PhysicalPageRef leafReference = allocator.allocatePage(Generation(1U));
		LocationLeafPage leaf;
		leaf.directory_type = directoryType;
		leaf.page_id = allocator.allocateMetadataPageId();
		leaf.generation = Generation(1U);
		leaf.committed_lsn = LogSequenceNumber(1U);
		leaf.entries = {{1U, SegmentId(1U), Generation(1U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, 0U}};
		pages.writeLocationLeaf(leafReference, leaf);

		const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1U));
		MetadataRadixPage root;
		root.kind = CatalogRadixPageKind::CRPK_ROOT;
		root.directory_type = directoryType;
		root.level = 0U;
		root.page_id = allocator.allocateMetadataPageId();
		root.generation = Generation(1U);
		root.committed_lsn = LogSequenceNumber(1U);
		root.children[0] = leafReference;
		pages.writeMetadataRadix(rootReference, root);
		assignLocationDirectoryRoot(superblock, directoryType, rootReference);
		return rootReference;
	}

} // namespace

TEST_F(RelocatableMetadataPagePoolTest, RecoveryValidatesNestedAllocatorSummaryAndFullLeaf) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	const PhysicalPageRef leafReference = allocator.allocatePage(Generation(1U));
	AllocatorLeafPage leaf;
	leaf.page_id = allocator.allocateMetadataPageId();
	leaf.first_slot = PhysicalSlotId(1U);
	leaf.slot_count = 8U;
	leaf.generation = Generation(1U);
	leaf.committed_lsn = LogSequenceNumber(1U);
	pages.writeAllocatorLeaf(leafReference, leaf);

	const PhysicalPageRef internalReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage internal = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_INTERNAL, 0U,
														 Generation(1U), LogSequenceNumber(1U));
	internal.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	internal.children[0] = leafReference;
	internal.allocator_free_children[0] = std::byte{0x01};
	pages.writeMetadataRadix(internalReference, internal);

	const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 1U, Generation(1U),
											  LogSequenceNumber(1U));
	root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	root.children[0] = internalReference;
	root.allocator_free_children[0] = std::byte{0x01};
	pages.writeMetadataRadix(rootReference, root);
	Superblock roots = initial;
	roots.allocator_root = rootReference;

	EXPECT_NO_THROW(allocator.recover(roots));
	root.allocator_free_children[0] = std::byte{};
	pages.writeMetadataRadix(rootReference, root);
	expectFormatError([&] { allocator.recover(roots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
	allocator.recover(initial);

	const PhysicalPageRef fullLeafReference = allocator.allocatePage(Generation(1U));
	AllocatorLeafPage fullLeaf;
	fullLeaf.page_id = allocator.allocateMetadataPageId();
	fullLeaf.first_slot = PhysicalSlotId(1U);
	fullLeaf.slot_count = 8U;
	fullLeaf.generation = Generation(1U);
	fullLeaf.committed_lsn = LogSequenceNumber(1U);
	fullLeaf.allocated_slots[0] = std::byte{0xFF};
	pages.writeAllocatorLeaf(fullLeafReference, fullLeaf);
	const PhysicalPageRef fullRootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage fullRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0U,
														 Generation(1U), LogSequenceNumber(1U));
	fullRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	fullRoot.children[0] = fullLeafReference;
	pages.writeMetadataRadix(fullRootReference, fullRoot);
	roots.allocator_root = fullRootReference;
	EXPECT_NO_THROW(allocator.recover(roots));

	const PhysicalPageRef partialLeafReference = allocator.allocatePage(Generation(1U));
	AllocatorLeafPage partialLeaf;
	partialLeaf.page_id = allocator.allocateMetadataPageId();
	partialLeaf.first_slot = PhysicalSlotId(1U);
	partialLeaf.slot_count = 7U;
	partialLeaf.generation = Generation(1U);
	partialLeaf.committed_lsn = LogSequenceNumber(1U);
	partialLeaf.allocated_slots[0] = std::byte{0x7F};
	pages.writeAllocatorLeaf(partialLeafReference, partialLeaf);
	const PhysicalPageRef partialRootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage partialRoot = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 0U,
														 Generation(1U), LogSequenceNumber(1U));
	partialRoot.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	partialRoot.children[0] = partialLeafReference;
	pages.writeMetadataRadix(partialRootReference, partialRoot);
	roots.allocator_root = partialRootReference;
	EXPECT_NO_THROW(allocator.recover(roots));
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryAcceptsACompletelyAllocatedAllocatorSubtree) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	MetadataRadixPage internal = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_INTERNAL, 0U,
														 Generation(1U), LogSequenceNumber(1U));
	internal.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
		const PhysicalPageRef leafReference = allocator.allocatePage(Generation(1U));
		AllocatorLeafPage leaf;
		leaf.page_id = allocator.allocateMetadataPageId();
		leaf.first_slot = PhysicalSlotId(static_cast<uint64_t>(index) * RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U);
		leaf.slot_count = 1U;
		leaf.generation = Generation(1U);
		leaf.committed_lsn = LogSequenceNumber(1U);
		leaf.allocated_slots[0] = std::byte{0x01};
		pages.writeAllocatorLeaf(leafReference, leaf);
		internal.children[index] = leafReference;
	}
	const PhysicalPageRef internalReference = allocator.allocatePage(Generation(1U));
	pages.writeMetadataRadix(internalReference, internal);

	const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1U));
	MetadataRadixPage root = radixPage(allocator.allocateMetadataPageId(), CatalogRadixPageKind::CRPK_ROOT, 1U, Generation(1U),
											  LogSequenceNumber(1U));
	root.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	root.children[0] = internalReference;
	pages.writeMetadataRadix(rootReference, root);
	Superblock roots = initial;
	roots.allocator_root = rootReference;

	EXPECT_NO_THROW(allocator.recover(roots));
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(RelocatableMetadataPagePoolTest, FaultInjectedRegistryDivergenceFailsClosedForEmergencyReserve) {
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
		reserve.pool_token = SegmentId(71U);
		reserve.generation = Generation(1U);
		reserve.committed_lsn = LogSequenceNumber(1U);
		allocator.createEmergencyReserve(reserve);

		allocator.eraseRegisteredPoolForTesting(reserve.physical_slot);
		EXPECT_FALSE(allocator.emergencyReserveHasAllocatedFrames());
		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnNormalPool(
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
		expectFormatError([&] { static_cast<void>(allocator.allocatePage(Generation(1U))); },
						  FormatError::FERR_INVALID_STATE);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, FaultInjectedReserveIdentityDivergenceRejectsRotation) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));
	MetadataEmergencyReserveDescriptor reserve;
	reserve.physical_slot = PhysicalSlotId(2U);
	reserve.pool_token = SegmentId(71U);
	reserve.generation = Generation(1U);
	reserve.committed_lsn = LogSequenceNumber(1U);
	allocator.createEmergencyReserve(reserve);

	MetadataPagePoolHeader corrupted = allocator.poolHeader(reserve.physical_slot);
	corrupted.generation = Generation(2U);
	allocator.replaceRegisteredPoolHeaderForTesting(reserve.physical_slot, corrupted);
	expectFormatError(
			[&] { static_cast<void>(allocator.rotateEmergencyReserve(SegmentId(72U), Generation(2U), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
}
#endif

TEST_F(RelocatableMetadataPagePoolTest, RootDiscoveryTraversesAnIndirectCatalogDirectory) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock first =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1U, 101U, 2U, 2U)}, LogSequenceNumber(2U));
	const uint64_t indirectSegmentId =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock second = tree.upsertAndPublish(bootstrap, first,
			std::vector{entry(indirectSegmentId, 102U, 3U, 3U)}, LogSequenceNumber(3U));

	EXPECT_NO_THROW(
			static_cast<void>(MetadataPagePoolAllocator(io, second, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY)));
}

TEST_F(RelocatableMetadataPagePoolTest, RootDiscoveryUsesBootstrapRoutesWhenPhysicalRootsAreAbsent) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	const Superblock populated = tree.upsertAndPublish(
			bootstrap, initial, std::vector{entry(1U, 101U, 2U, 2U)}, LogSequenceNumber(2U));
	MetadataPagePoolAllocator migrationAllocator(io, populated);
	MetadataPageFile migrationPages(io, populated);
	MetadataWriteBatch migration(migrationAllocator, populated, LogSequenceNumber(3U));
	static_cast<void>(MetadataPageRouteDirectory(migrationPages).prepareInitialBootstrapMigration(migration));
	migration.publish(bootstrap);
	Superblock routed = migration.candidate();
	routed.catalog_root = {};
	routed.allocator_root = {};

	EXPECT_NO_THROW(static_cast<void>(MetadataPagePoolAllocator(io, routed, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER)));
}

TEST_F(RelocatableMetadataPagePoolTest, DiscoveryOnlyOpenAcceptsAnEmptyMetadataTail) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);

	EXPECT_NO_THROW(
			static_cast<void>(MetadataPagePoolAllocator(io, initial, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY)));
}

TEST_F(RelocatableMetadataPagePoolTest, RootDiscoveryRejectsHeaderSegmentIdentityMismatch) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	Superblock roots = initial;
	const PhysicalPageRef root = writeLocationDirectoryRoot(pages, allocator, MetadataDirectoryType::MDT_PAGE, roots);
	const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES +
			(root.physical_slot.value() - 1U) * static_cast<uint64_t>(RSS_SEGMENT_SIZE);
	MetadataPagePoolHeader header = allocator.poolHeader(root.physical_slot);
	header.segment_id = SegmentId(header.segment_id.value() + 1U);
	writeControlHeaderCopies(*io, poolOffset, header);
	io->sync();

	expectFormatError(
			[&] { static_cast<void>(MetadataPagePoolAllocator(io, roots, MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataPagePoolTest, UnknownOrNonemptyBatchPoolsFailClosedBeforeReuse) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	EXPECT_TRUE(allocator.poolSlots().empty());

	const PhysicalPageRef allocated = allocator.allocatePage(Generation(1U));
	allocator.abandonUnpublishedBatch(std::array{allocated.physical_slot}, {}, io->fileSize());
	EXPECT_TRUE(allocator.stats().poisoned);
	EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), FormatException);
}

TEST_F(RelocatableMetadataPagePoolTest, RecoveryRejectsMalformedSuperblockAndLocationRoutes) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);

	Superblock invalidRoot = initial;
	invalidRoot.catalog_root = PhysicalPageRef{PhysicalSlotId(1U), 0U, Generation(1U)};
	expectFormatError([&] { allocator.recover(invalidRoot); }, FormatError::FERR_INVALID_HEADER);
	// Superblock validation rejects an invalid physical reference before the
	// allocator touches mutable state, so this is not an allocator poison event.
	EXPECT_FALSE(allocator.stats().poisoned);
	expectFormatError(
			[&] {
				static_cast<void>(MetadataPagePoolAllocator(io, invalidRoot,
																  MetadataPagePoolOpenMode::CPOM_ROOT_DISCOVER_ONLY));
			},
			FormatError::FERR_INVALID_HEADER);

	Superblock roots = initial;
	const PhysicalPageRef root = writeLocationDirectoryRoot(pages, allocator, MetadataDirectoryType::MDT_PAGE, roots);
	const MetadataRadixPage radix = pages.readMetadataRadix(root);
	LocationLeafPage leaf = pages.readLocationLeaf(radix.children[0]);
	leaf.entries[0].logical_id = RSS_LOCATION_LEAF_MAX_ENTRIES + 1U;
	pages.writeLocationLeaf(radix.children[0], leaf);
	expectFormatError([&] { allocator.recover(roots); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.stats().poisoned);
}

TEST_F(RelocatableMetadataPagePoolTest, EmergencyReserveCannotRotateWhenNoReserveExists) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);

	expectFormatError(
			[&] { static_cast<void>(allocator.rotateEmergencyReserve(SegmentId(71U), Generation(2U), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
}
