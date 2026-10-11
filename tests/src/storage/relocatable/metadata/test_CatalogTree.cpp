/**
 * @file test_CatalogTree.cpp
 * @brief COW publication, recovery, and route tests for relocatable Catalog trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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

	CatalogEntry entry(uint64_t segmentId, uint64_t physicalSlot, uint64_t generation, uint64_t lsn) {
		CatalogEntry result;
		result.segment_id = SegmentId(segmentId);
		result.physical_slot = PhysicalSlotId(physicalSlot);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 7;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	void expectSameSuperblock(const Superblock &actual, const Superblock &expected) {
		EXPECT_EQ(actual.generation, expected.generation);
		EXPECT_EQ(actual.checkpoint_lsn, expected.checkpoint_lsn);
		EXPECT_EQ(actual.data_region_offset, expected.data_region_offset);
		EXPECT_EQ(actual.catalog_root, expected.catalog_root);
		EXPECT_EQ(actual.page_directory_root, expected.page_directory_root);
		EXPECT_EQ(actual.extent_directory_root, expected.extent_directory_root);
		EXPECT_EQ(actual.allocator_root, expected.allocator_root);
	}

	class TestMetadataPageAllocator final : public MetadataPageAllocator {
	public:
		MetadataPageId allocateMetadataPageId() override {
			if (invalidMetadataPageId) {
				return MetadataPageId{};
			}
			return MetadataPageId(nextMetadataPageId++);
		}

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
											std::span<const PhysicalSlotId>) override {
			if (invalidPhysicalReference) {
				return {PhysicalPageRef{}, std::nullopt};
			}
			if (duplicatePhysicalReference && !returnedReferences.empty()) {
				return {returnedReferences.front(), std::nullopt};
			}
			const uint64_t index = nextPhysicalPage++;
			PhysicalPageRef reference;
			reference.physical_slot = PhysicalSlotId(index / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
			reference.frame_index = static_cast<uint8_t>(index % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U);
			reference.generation = wrongPhysicalGeneration ? Generation(generation.value() + 1U) : generation;
			returnedReferences.push_back(reference);
			return {reference, std::nullopt};
		}

		void abandonPage(PhysicalPageRef reference) noexcept override { abandonedReferences.push_back(reference); }

		bool invalidMetadataPageId = false;
		bool invalidPhysicalReference = false;
		bool wrongPhysicalGeneration = false;
		bool duplicatePhysicalReference = false;
		uint64_t nextMetadataPageId = 1;
		uint64_t nextPhysicalPage = 0;
		std::vector<PhysicalPageRef> returnedReferences;
		std::vector<PhysicalPageRef> abandonedReferences;
	};

	TEST(MetadataPageAllocationContractTest, ConvertsToItsPhysicalPageAndSupportsDefaultPoolAbandonment) {
		TestMetadataPageAllocator allocator;
		MetadataPageAllocation allocation;
		allocation.page = {PhysicalSlotId(7U), 3U, Generation(11U)};
		allocation.created_pool_slot = PhysicalSlotId(7U);

		EXPECT_EQ(static_cast<PhysicalPageRef>(allocation), allocation.page);
		MetadataPageAllocator &baseAllocator = allocator;
		EXPECT_NO_THROW(baseAllocator.abandonUnpublishedPool(PhysicalSlotId(7U)));
	}

} // namespace

class RelocatableCatalogTreeTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("relocatable_catalog_tree_" + to_string(uuid) + ".dat");
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

TEST_F(RelocatableCatalogTreeTest, PublishesOneBatchIntoSharedLeavesAndSurvivesReopen) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const std::vector<CatalogEntry> entries = {entry(1, 100, 1, 2), entry(2, 101, 1, 2),
											   entry(RSS_CATALOG_PAGE_MAX_ENTRIES, 102, 1, 2),
											   entry(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U, 103, 1, 2)};

	const Superblock published = tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2));
	EXPECT_EQ(published.generation, Generation(2));
	EXPECT_EQ(published.checkpoint_lsn, LogSequenceNumber(2));
	EXPECT_TRUE(published.catalog_root.isValid());
	EXPECT_EQ(allocator.returnedReferences.size(), 3U); // Two leaves and their shared level-zero root.
	EXPECT_EQ(tree.find(published, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(published, SegmentId(2)).physical_slot, PhysicalSlotId(101));
	EXPECT_EQ(tree.find(published, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES)).physical_slot, PhysicalSlotId(102));
	EXPECT_EQ(tree.find(published, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U)).physical_slot, PhysicalSlotId(103));

	const CatalogRadixPage root = pages.readCatalogRadix(published.catalog_root);
	ASSERT_EQ(root.level, 0U);
	EXPECT_TRUE(root.children[0].isValid());
	EXPECT_TRUE(root.children[1].isValid());
	EXPECT_EQ(pages.readCatalogLeaf(root.children[0]).entries.size(), 3U);
	EXPECT_EQ(pages.readCatalogLeaf(root.children[1]).entries.size(), 1U);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	CatalogTree reopenedTree(reopenedPages, allocator);
	expectSameSuperblock(recovered, published);
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U)).physical_slot,
			  PhysicalSlotId(103));
}

TEST_F(RelocatableCatalogTreeTest, NativeCandidateShortWriteRetainsOldGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		const PhysicalPageRef candidateLeaf{PhysicalSlotId(1U), 1U, Generation(2U)};
		const uint64_t candidateOffset = pages.pageOffset(candidateLeaf);
		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[candidateOffset, &shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																						pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_CATALOG_PAGE_HEADER_BYTES - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);
		expectSameSuperblock(bootstrap.recover(), initial);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectSameSuperblock(recovered, superblock(1U, 1U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	const Superblock published = reopenedTree.upsertAndPublish(
			reopenedBootstrap, recovered, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));
	EXPECT_EQ(reopenedTree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeCandidateDiskFullRetainsOldGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		const PhysicalPageRef candidateLeaf{PhysicalSlotId(1U), 1U, Generation(2U)};
		const uint64_t candidateOffset = pages.pageOffset(candidateLeaf);
		bool diskFullObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
					[candidateOffset, &diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																						pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							diskFullObserved = true;
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::system_error);
		}
		EXPECT_TRUE(diskFullObserved);
		expectSameSuperblock(bootstrap.recover(), initial);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectSameSuperblock(recovered, superblock(1U, 1U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	const Superblock published = reopenedTree.upsertAndPublish(
			reopenedBootstrap, recovered, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));
	EXPECT_EQ(reopenedTree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeRadixCandidateShortWriteRetainsOldGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		const PhysicalPageRef candidateRoot{PhysicalSlotId(1U), 2U, Generation(2U)};
		const uint64_t candidateOffset = pages.pageOffset(candidateRoot);
		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[candidateOffset, &shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_CATALOG_PAGE_HEADER_BYTES - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);
		expectSameSuperblock(bootstrap.recover(), initial);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectSameSuperblock(recovered, superblock(1U, 1U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	const Superblock published = reopenedTree.upsertAndPublish(
			reopenedBootstrap, recovered, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));
	EXPECT_EQ(reopenedTree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeRadixCandidateDiskFullRetainsOldGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		const PhysicalPageRef candidateRoot{PhysicalSlotId(1U), 2U, Generation(2U)};
		const uint64_t candidateOffset = pages.pageOffset(candidateRoot);
		bool diskFullObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
					[candidateOffset, &diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount,
															pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							diskFullObserved = true;
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::system_error);
		}
		EXPECT_TRUE(diskFullObserved);
		expectSameSuperblock(bootstrap.recover(), initial);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectSameSuperblock(recovered, superblock(1U, 1U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	const Superblock published = reopenedTree.upsertAndPublish(
			reopenedBootstrap, recovered, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));
	EXPECT_EQ(reopenedTree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeDataFenceFailureRetainsOldGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failDataFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		expectSameSuperblock(bootstrap.recover(), initial);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	expectSameSuperblock(recovered, superblock(1U, 1U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	const Superblock published = reopenedTree.upsertAndPublish(
			reopenedBootstrap, recovered, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));
	EXPECT_EQ(reopenedTree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeFinalFenceFailureReopensTheCompletePublishedGeneration) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const std::vector<CatalogEntry> entries = {entry(1U, 100U, 1U, 2U)};
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, entries, LogSequenceNumber(2U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		const Superblock published = bootstrap.recover();
		EXPECT_EQ(published.generation, Generation(2U));
		EXPECT_EQ(tree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, PreservesTheOldRootWhileCopyingOnlyAffectedPaths) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const std::vector<CatalogEntry> firstBatch = {entry(1, 100, 1, 2),
												  entry(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U, 101, 1, 2)};
	const Superblock first = tree.upsertAndPublish(bootstrap, initial, firstBatch, LogSequenceNumber(2));
	const CatalogRadixPage oldRoot = pages.readCatalogRadix(first.catalog_root);
	ASSERT_EQ(oldRoot.level, 0U);

	const std::vector<CatalogEntry> replacement = {entry(1, 200, 2, 3)};
	const Superblock second = tree.upsertAndPublish(bootstrap, first, replacement, LogSequenceNumber(3));
	const CatalogRadixPage newRoot = pages.readCatalogRadix(second.catalog_root);
	EXPECT_NE(second.catalog_root, first.catalog_root);
	EXPECT_NE(newRoot.children[0], oldRoot.children[0]);
	EXPECT_EQ(newRoot.children[1], oldRoot.children[1]);
	EXPECT_EQ(allocator.returnedReferences.size(), 5U); // Initial: root + 2 leaves; replacement: root + 1 leaf.
	EXPECT_EQ(tree.find(first, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(second, SegmentId(1)).physical_slot, PhysicalSlotId(200));
	EXPECT_EQ(tree.find(second, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U)).physical_slot, PhysicalSlotId(101));
}

TEST_F(RelocatableCatalogTreeTest, GrowsTheRootWithoutRewritingUnchangedLeaves) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const Superblock first =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const CatalogRadixPage oldRoot = pages.readCatalogRadix(first.catalog_root);
	const uint64_t firstSegmentInSecondRootChild =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock second = tree.upsertAndPublish(
			bootstrap, first, std::vector{entry(firstSegmentInSecondRootChild, 200, 1, 3)}, LogSequenceNumber(3));
	const CatalogRadixPage newRoot = pages.readCatalogRadix(second.catalog_root);
	ASSERT_EQ(newRoot.level, 1U);
	ASSERT_TRUE(newRoot.children[0].isValid());
	ASSERT_TRUE(newRoot.children[1].isValid());
	const CatalogRadixPage lowerZero = pages.readCatalogRadix(newRoot.children[0]);
	EXPECT_EQ(lowerZero.kind, CatalogRadixPageKind::CRPK_INTERNAL);
	EXPECT_EQ(lowerZero.level, 0U);
	EXPECT_NE(newRoot.children[0], first.catalog_root);
	EXPECT_EQ(tree.find(second, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(second, SegmentId(firstSegmentInSecondRootChild)).physical_slot, PhysicalSlotId(200));
	EXPECT_EQ(oldRoot.children[0], lowerZero.children[0]);
	EXPECT_EQ(allocator.returnedReferences.size(), 6U); // First root + leaf, then leaf + 2 level-zero nodes + root.
	expectFormatError([&] { static_cast<void>(tree.find(first, SegmentId(firstSegmentInSecondRootChild))); },
					  FormatError::FERR_INVALID_HEADER);

	const Superblock third = tree.upsertAndPublish(
			bootstrap, second, std::vector{entry(firstSegmentInSecondRootChild, 201, 2, 4)}, LogSequenceNumber(4));
	EXPECT_EQ(tree.find(third, SegmentId(firstSegmentInSecondRootChild)).physical_slot, PhysicalSlotId(201));
	EXPECT_EQ(allocator.returnedReferences.size(), 9U); // Leaf, loaded internal node, and root are copied once.
}

TEST_F(RelocatableCatalogTreeTest, StreamsNewCatalogEntriesAcrossLeaves) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const Superblock before =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));

	const uint64_t first = RSS_CATALOG_PAGE_MAX_ENTRIES;
	const std::array additions = {entry(first, 101U, 1U, 3U), entry(first + 1U, 102U, 1U, 3U),
									 entry(first + 2U, 103U, 1U, 3U)};
	size_t cursor = 0U;
	MetadataWriteBatch batch(allocator, before, LogSequenceNumber(3U));
	tree.prepareAppend(batch, SegmentId(first), SegmentId(first + additions.size() - 1U), additions.size(),
			[&]() -> std::optional<CatalogEntry> {
				return cursor == additions.size() ? std::nullopt : std::optional<CatalogEntry>(additions[cursor++]);
			});
	batch.publish(bootstrap);
	const Superblock after = batch.candidate();

	EXPECT_FALSE(tree.findOptional(before, additions.front().segment_id).has_value());
	EXPECT_EQ(tree.find(after, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
	for (const CatalogEntry &catalogEntry: additions) {
		EXPECT_EQ(tree.find(after, catalogEntry.segment_id).physical_slot, catalogEntry.physical_slot);
	}
}

TEST_F(RelocatableCatalogTreeTest, RecordsFinalCandidateFramesForEveryCatalogCowPage) {
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	tree.prepareUpsert(batch, std::vector{entry(1U, 100U, 1U, 2U)});
	const Superblock candidate = batch.candidate();
	const CatalogRadixPage root = pages.readCatalogRadix(candidate.catalog_root);
	ASSERT_TRUE(root.children[0].isValid());
	const CatalogLeafPage leaf = pages.readCatalogLeaf(root.children[0]);
	const std::vector<MetadataPageRoute> routes = batch.metadataPageRoutes();
	ASSERT_EQ(routes.size(), 2U);
	EXPECT_EQ(routes[0], (MetadataPageRoute{root.page_id, candidate.catalog_root}));
	EXPECT_EQ(routes[1], (MetadataPageRoute{leaf.page_id, root.children[0]}));
}

TEST_F(RelocatableCatalogTreeTest, RejectsAStreamThatDivergesFromItsDeclaredSegmentRange) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	bool emitted = false;
	expectFormatError(
			[&] {
				tree.prepareAppend(batch, SegmentId(10U), SegmentId(11U), 2U,
								   [&]() -> std::optional<CatalogEntry> {
									   if (emitted) {
										   return std::nullopt;
									   }
									   emitted = true;
									   return entry(11U, 100U, 1U, 2U);
								   });
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableCatalogTreeTest, AcceptsAProducerWithoutAnIntermediateCatalogVector) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2U));
	tree.prepareAppend(batch, SegmentId(8U), SegmentId(9U), 2U, [](const CatalogTree::EntrySink &consume) {
		consume(entry(8U, 101U, 1U, 2U));
		consume(entry(9U, 102U, 1U, 2U));
	});
	batch.publish(bootstrap);
	const Superblock published = batch.candidate();

	EXPECT_EQ(tree.find(published, SegmentId(8U)).physical_slot, PhysicalSlotId(101U));
	EXPECT_EQ(tree.find(published, SegmentId(9U)).physical_slot, PhysicalSlotId(102U));
}

TEST_F(RelocatableCatalogTreeTest, PreservesExistingSparseSubtreeAcrossMultiLevelStreamedRootGrowth) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const Superblock before =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1U, 100U, 1U, 2U)}, LogSequenceNumber(2U));

	const uint64_t distantId = static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) *
								   RSS_CATALOG_RADIX_FANOUT * RSS_CATALOG_RADIX_FANOUT + 1U;
	const CatalogEntry distant = entry(distantId, 200U, 1U, 3U);
	bool emitted = false;
	MetadataWriteBatch batch(allocator, before, LogSequenceNumber(3U));
	tree.prepareAppend(batch, SegmentId(distantId), SegmentId(distantId), 1U,
			[&]() -> std::optional<CatalogEntry> {
				if (emitted) {
					return std::nullopt;
				}
				emitted = true;
				return distant;
			});
	batch.publish(bootstrap);
	const Superblock after = batch.candidate();

	EXPECT_EQ(tree.find(before, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
	EXPECT_FALSE(tree.findOptional(before, distant.segment_id).has_value());
	EXPECT_EQ(tree.find(after, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
	EXPECT_EQ(tree.find(after, distant.segment_id).physical_slot, distant.physical_slot);
}

TEST_F(RelocatableCatalogTreeTest, RejectsInvalidBatchesAndStaleSuperblocksBeforeAllocatingPages) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const std::vector<CatalogEntry> unordered = {entry(2, 100, 1, 2), entry(1, 101, 1, 2)};
	expectFormatError(
			[&] { static_cast<void>(tree.upsertAndPublish(bootstrap, initial, unordered, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	EXPECT_TRUE(allocator.returnedReferences.empty());

	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const size_t allocationsBeforeStalePublication = allocator.returnedReferences.size();
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(2, 101, 1, 3)},
														LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(allocator.returnedReferences.size(), allocationsBeforeStalePublication);
	expectSameSuperblock(bootstrap.recover(), published);
}

TEST_F(RelocatableCatalogTreeTest, AbandonsOnlyUnpublishedPagesAcrossPublicationCrashBoundaries) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const std::vector<CatalogEntry> firstBatch = {entry(1, 100, 1, 2)};
	EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, firstBatch, LogSequenceNumber(2),
														 BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	expectSameSuperblock(bootstrap.recover(), initial);
	ASSERT_EQ(allocator.returnedReferences.size(), 2U);
	EXPECT_EQ(allocator.abandonedReferences.size(), 2U);

	const std::vector<CatalogEntry> secondBatch = {entry(1, 101, 1, 2)};
	EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, secondBatch, LogSequenceNumber(2),
														 BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	expectSameSuperblock(bootstrap.recover(), initial);
	EXPECT_EQ(allocator.abandonedReferences.size(), 4U);

	const std::vector<CatalogEntry> thirdBatch = {entry(1, 102, 1, 2)};
	EXPECT_THROW(static_cast<void>(tree.upsertAndPublish(bootstrap, initial, thirdBatch, LogSequenceNumber(2),
														 BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	const Superblock recovered = bootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2));
	EXPECT_EQ(tree.find(recovered, SegmentId(1)).physical_slot, PhysicalSlotId(102));
	EXPECT_EQ(allocator.abandonedReferences.size(), 4U);
}

TEST_F(RelocatableCatalogTreeTest, RejectsBadAllocatorReferencesAndInvalidLookupsWithoutPublishing) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	expectFormatError([&] { static_cast<void>(tree.find(initial, SegmentId(1))); }, FormatError::FERR_INVALID_STATE);

	allocator.invalidMetadataPageId = true;
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
														LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	expectSameSuperblock(bootstrap.recover(), initial);

	allocator.invalidMetadataPageId = false;
	allocator.invalidPhysicalReference = true;
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
														LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	expectSameSuperblock(bootstrap.recover(), initial);

	allocator.invalidPhysicalReference = false;
	allocator.duplicatePhysicalReference = true;
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
														LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	expectSameSuperblock(bootstrap.recover(), initial);
	EXPECT_EQ(allocator.abandonedReferences.size(), 1U);
}

TEST_F(RelocatableCatalogTreeTest, FindsOptionalEntriesAcrossEmptySparseAndOutOfRangeRoutes) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	EXPECT_FALSE(tree.findOptional(initial, SegmentId(1)).has_value());
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	EXPECT_EQ(tree.findOptional(published, SegmentId(1))->physical_slot, PhysicalSlotId(100));
	EXPECT_FALSE(tree.findOptional(published, SegmentId(2)).has_value());
	EXPECT_FALSE(tree.findOptional(published, SegmentId(static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) *
																RSS_CATALOG_RADIX_FANOUT +
														1U))
						 .has_value());
	expectFormatError([&] { static_cast<void>(tree.findOptional(published, SegmentId{})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableCatalogTreeTest, VisitsEveryPublishedEntryAndRejectsDuplicatePhysicalPages) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	bool visitedEmptyTree = false;
	tree.visit(initial, [&](const CatalogEntry &) { visitedEmptyTree = true; });
	EXPECT_FALSE(visitedEmptyTree);
	const std::function<void(const CatalogEntry &)> emptyVisitor;
	expectFormatError([&] { tree.visit(initial, emptyVisitor); }, FormatError::FERR_INVALID_HEADER);

	const uint64_t deepSegmentId =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock published = tree.upsertAndPublish(
			bootstrap, initial,
			std::vector{entry(1, 100, 1, 2), entry(3, 101, 1, 2),
						entry(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U, 102, 1, 2), entry(deepSegmentId, 103, 1, 2)},
			LogSequenceNumber(2));
	std::vector<SegmentId> visited;
	tree.visit(published, [&](const CatalogEntry &current) { visited.push_back(current.segment_id); });
	EXPECT_EQ(visited, (std::vector<SegmentId>{SegmentId(1), SegmentId(3),
																						 SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U),
																						 SegmentId(deepSegmentId)}));
	EXPECT_FALSE(tree.findOptional(published, SegmentId(2)).has_value());

	const CatalogRadixPage originalRoot = pages.readCatalogRadix(published.catalog_root);
	ASSERT_EQ(originalRoot.level, 1U);
	ASSERT_TRUE(originalRoot.children[0].isValid());
	ASSERT_TRUE(originalRoot.children[1].isValid());
	CatalogRadixPage duplicateRoot = originalRoot;
	duplicateRoot.children[1] = duplicateRoot.children[0];
	pages.writeCatalogRadix(published.catalog_root, duplicateRoot);
	expectFormatError([&] { tree.visit(published, [](const CatalogEntry &) {}); }, FormatError::FERR_INVALID_STATE);
	pages.writeCatalogRadix(published.catalog_root, originalRoot);

	CatalogRadixPage wrongLevel = pages.readCatalogRadix(originalRoot.children[1]);
	++wrongLevel.level;
	pages.writeCatalogRadix(originalRoot.children[1], wrongLevel);
	expectFormatError([&] { tree.visit(published, [](const CatalogEntry &) {}); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableCatalogTreeTest, FindsOptionalEntriesThroughADeepRadixRouteAndRejectsForeignBatches) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const uint64_t deepSegmentId =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2), entry(deepSegmentId, 101, 1, 2)},
								  LogSequenceNumber(2));
	const std::optional<CatalogEntry> found = tree.findOptional(published, SegmentId(deepSegmentId));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->physical_slot, PhysicalSlotId(101));

	TestMetadataPageAllocator foreignAllocator;
	MetadataWriteBatch foreignBatch(foreignAllocator, published, LogSequenceNumber(3));
	expectFormatError([&] { tree.prepareUpsert(foreignBatch, std::vector{entry(2, 102, 1, 3)}); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableCatalogTreeTest, RejectsMalformedMutationsBeforeWritingMetadata) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);

	expectFormatError([&] { static_cast<void>(tree.upsertAndPublish(bootstrap, initial, {}, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
														LogSequenceNumber(1)));
			},
			FormatError::FERR_INVALID_STATE);

	CatalogEntry badEnum = entry(1, 100, 1, 2);
	badEnum.kind = static_cast<SegmentKind>(99);
	expectFormatError(
			[&] {
				static_cast<void>(
						tree.upsertAndPublish(bootstrap, initial, std::vector{badEnum}, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_ENUM);
	CatalogEntry badState = entry(1, 100, 1, 2);
	badState.state = SegmentState::SGS_BUILDING;
	expectFormatError(
			[&] {
				static_cast<void>(
						tree.upsertAndPublish(bootstrap, initial, std::vector{badState}, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	CatalogEntry badIdentity = entry(1, 100, 1, 2);
	badIdentity.physical_slot = PhysicalSlotId{};
	expectFormatError(
			[&] {
				static_cast<void>(
						tree.upsertAndPublish(bootstrap, initial, std::vector{badIdentity}, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	const auto otherIo = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	MetadataPageFile otherPages(otherIo, initial);
	CatalogTree otherTree(otherPages, allocator);
	expectFormatError(
			[&] {
				static_cast<void>(otherTree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)},
															 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(allocator.returnedReferences.empty());
	expectSameSuperblock(bootstrap.recover(), initial);
}

TEST_F(RelocatableCatalogTreeTest, RejectsCorruptPublishedRootsLeavesAndMissingEntries) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const Superblock published = tree.upsertAndPublish(
			bootstrap, initial, std::vector{entry(1, 100, 1, 2), entry(3, 101, 1, 2)}, LogSequenceNumber(2));
	const CatalogRadixPage validRoot = pages.readCatalogRadix(published.catalog_root);

	expectFormatError([&] { static_cast<void>(tree.find(published, SegmentId(2))); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(tree.find(published, SegmentId{})); }, FormatError::FERR_INVALID_STATE);

	CatalogRadixPage wrongKindRoot = validRoot;
	wrongKindRoot.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	pages.writeCatalogRadix(published.catalog_root, wrongKindRoot);
	expectFormatError([&] { static_cast<void>(tree.find(published, SegmentId(1))); }, FormatError::FERR_INVALID_HEADER);
	pages.writeCatalogRadix(published.catalog_root, validRoot);

	Superblock staleGeneration = published;
	staleGeneration.generation = Generation(1);
	expectFormatError([&] { static_cast<void>(tree.find(staleGeneration, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	Superblock staleLsn = published;
	staleLsn.checkpoint_lsn = LogSequenceNumber(1);
	expectFormatError([&] { static_cast<void>(tree.find(staleLsn, SegmentId(1))); }, FormatError::FERR_INVALID_STATE);

	CatalogLeafPage misplaced;
	misplaced.page_id = MetadataPageId(999);
	misplaced.minimum_segment_id = SegmentId(85);
	misplaced.maximum_segment_id = SegmentId(85);
	misplaced.generation = Generation(2);
	misplaced.committed_lsn = LogSequenceNumber(2);
	misplaced.entries = {entry(85, 200, 1, 2)};
	pages.writeCatalogLeaf(validRoot.children[0], misplaced);
	expectFormatError([&] { static_cast<void>(tree.find(published, SegmentId(1))); }, FormatError::FERR_INVALID_HEADER);

	CatalogLeafPage empty;
	empty.page_id = MetadataPageId(1000);
	empty.generation = Generation(2);
	empty.committed_lsn = LogSequenceNumber(2);
	pages.writeCatalogLeaf(validRoot.children[0], empty);
	expectFormatError([&] { static_cast<void>(tree.find(published, SegmentId(1))); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableCatalogTreeTest, RejectsCorruptInternalNodesAndAllocatorGenerationMismatches) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const uint64_t highSegmentId =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock first =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const Superblock second =
			tree.upsertAndPublish(bootstrap, first, std::vector{entry(highSegmentId, 200, 1, 3)}, LogSequenceNumber(3));
	const CatalogRadixPage root = pages.readCatalogRadix(second.catalog_root);
	CatalogRadixPage badChild = pages.readCatalogRadix(root.children[1]);
	badChild.kind = CatalogRadixPageKind::CRPK_ROOT;
	pages.writeCatalogRadix(root.children[1], badChild);
	expectFormatError([&] { static_cast<void>(tree.find(second, SegmentId(highSegmentId))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, second, std::vector{entry(highSegmentId, 201, 2, 4)},
														LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_HEADER);

	badChild.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	badChild.level = 1;
	pages.writeCatalogRadix(root.children[1], badChild);
	expectFormatError([&] { static_cast<void>(tree.find(second, SegmentId(highSegmentId))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(tree.upsertAndPublish(bootstrap, second, std::vector{entry(highSegmentId, 201, 2, 4)},
														LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_HEADER);

	const Superblock clean = superblock(1, 1);
	SuperblockFile cleanBootstrap(io);
	cleanBootstrap.initializeNew(clean);
	MetadataPageFile cleanPages(io, clean);
	TestMetadataPageAllocator badAllocator;
	badAllocator.wrongPhysicalGeneration = true;
	CatalogTree badTree(cleanPages, badAllocator);
	expectFormatError(
			[&] {
				static_cast<void>(badTree.upsertAndPublish(cleanBootstrap, clean, std::vector{entry(1, 300, 1, 2)},
														   LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableCatalogTreeTest, ErasesEntriesWithCopyOnWriteAndPrunesEmptyBranches) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const uint64_t deepSegmentId =
			static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U;
	const Superblock first = tree.upsertAndPublish(
			bootstrap, initial,
			std::vector{entry(1, 100, 1, 2), entry(2, 101, 1, 2), entry(deepSegmentId, 102, 1, 2)},
			LogSequenceNumber(2));
	const CatalogRadixPage oldRoot = pages.readCatalogRadix(first.catalog_root);
	ASSERT_EQ(oldRoot.level, 1U);

	MetadataWriteBatch batch(allocator, first, LogSequenceNumber(3));
	const std::array removed{SegmentId(1), SegmentId(deepSegmentId)};
	tree.prepareErase(batch, removed);
	batch.publish(bootstrap);
	const Superblock second = batch.candidate();
	const CatalogRadixPage newRoot = pages.readCatalogRadix(second.catalog_root);
	EXPECT_NE(second.catalog_root, first.catalog_root);
	EXPECT_TRUE(newRoot.children[0].isValid());
	EXPECT_FALSE(newRoot.children[1].isValid());
	EXPECT_FALSE(tree.findOptional(second, SegmentId(1)).has_value());
	EXPECT_EQ(tree.find(second, SegmentId(2)).physical_slot, PhysicalSlotId(101));
	EXPECT_FALSE(tree.findOptional(second, SegmentId(deepSegmentId)).has_value());

	std::vector<SegmentId> visited;
	tree.visit(second, [&visited](const CatalogEntry &entry) { visited.push_back(entry.segment_id); });
	EXPECT_EQ(visited, (std::vector<SegmentId>{SegmentId(2)}));
	EXPECT_EQ(tree.find(first, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(first, SegmentId(deepSegmentId)).physical_slot, PhysicalSlotId(102));

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	CatalogTree reopenedTree(reopenedPages, allocator);
	EXPECT_FALSE(reopenedTree.findOptional(recovered, SegmentId(1)).has_value());
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(2)).physical_slot, PhysicalSlotId(101));
	EXPECT_FALSE(reopenedTree.findOptional(recovered, SegmentId(deepSegmentId)).has_value());
}

TEST_F(RelocatableCatalogTreeTest, NativeCatalogUpsertUsesExactlyOneDataFenceAndOneSuperblockFence) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		std::atomic<uint32_t> syncCalls{0U};
		Superblock published;
		{
			StorageIO::ScopedNativeSyncHookForTesting countPublicationSyncs([&syncCalls](file_handle_t fd) {
				syncCalls.fetch_add(1U, std::memory_order_relaxed);
				return portable_fsync(fd);
			});
			published = tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1U, 100U, 1U, 2U)},
					LogSequenceNumber(2U));
		}
		EXPECT_EQ(syncCalls.load(std::memory_order_relaxed), 2U);
		EXPECT_EQ(tree.find(published, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(1U)).physical_slot, PhysicalSlotId(100U));
}

TEST_F(RelocatableCatalogTreeTest, NativeCatalogEraseUsesExactlyOneDataFenceAndOneSuperblockFence) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		TestMetadataPageAllocator allocator;
		CatalogTree tree(pages, allocator);
		const Superblock first = tree.upsertAndPublish(
				bootstrap, initial, std::vector{entry(1U, 100U, 1U, 2U), entry(2U, 101U, 1U, 2U)},
				LogSequenceNumber(2U));

		MetadataWriteBatch eraseBatch(allocator, first, LogSequenceNumber(3U));
		tree.prepareErase(eraseBatch, std::array{SegmentId(1U)});
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting countPublicationSyncs([&syncCalls](file_handle_t fd) {
				syncCalls.fetch_add(1U, std::memory_order_relaxed);
				return portable_fsync(fd);
			});
			eraseBatch.publish(bootstrap);
		}
		EXPECT_EQ(syncCalls.load(std::memory_order_relaxed), 2U);
		EXPECT_FALSE(tree.findOptional(eraseBatch.candidate(), SegmentId(1U)).has_value());
		EXPECT_EQ(tree.find(eraseBatch.candidate(), SegmentId(2U)).physical_slot, PhysicalSlotId(101U));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(3U));
	MetadataPageFile reopenedPages(io, recovered);
	TestMetadataPageAllocator allocator;
	CatalogTree reopenedTree(reopenedPages, allocator);
	EXPECT_FALSE(reopenedTree.findOptional(recovered, SegmentId(1U)).has_value());
	EXPECT_EQ(reopenedTree.find(recovered, SegmentId(2U)).physical_slot, PhysicalSlotId(101U));
}

TEST_F(RelocatableCatalogTreeTest, RejectsMalformedOrAbsentEraseRequestsWithoutPublication) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	TestMetadataPageAllocator allocator;
	CatalogTree tree(pages, allocator);
	const Superblock published =
			tree.upsertAndPublish(bootstrap, initial, std::vector{entry(1, 100, 1, 2)}, LogSequenceNumber(2));
	const size_t allocatedBefore = allocator.returnedReferences.size();

	MetadataWriteBatch emptyBatch(allocator, published, LogSequenceNumber(3));
	expectFormatError([&] { tree.prepareErase(emptyBatch, {}); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(allocator.returnedReferences.size(), allocatedBefore);

	MetadataWriteBatch unorderedBatch(allocator, published, LogSequenceNumber(3));
	const std::array unordered{SegmentId(2), SegmentId(1)};
	expectFormatError([&] { tree.prepareErase(unorderedBatch, unordered); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(allocator.returnedReferences.size(), allocatedBefore);

	MetadataWriteBatch absentBatch(allocator, published, LogSequenceNumber(3));
	const std::array absent{SegmentId(2)};
	expectFormatError([&] { tree.prepareErase(absentBatch, absent); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(allocator.returnedReferences.size(), allocatedBefore);
	expectSameSuperblock(bootstrap.recover(), published);

	TestMetadataPageAllocator foreignAllocator;
	MetadataWriteBatch foreignBatch(foreignAllocator, published, LogSequenceNumber(3));
	const std::array existing{SegmentId(1)};
	expectFormatError([&] { tree.prepareErase(foreignBatch, existing); }, FormatError::FERR_INVALID_STATE);
}
