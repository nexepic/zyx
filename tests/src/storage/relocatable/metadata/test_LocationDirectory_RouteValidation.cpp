/**
 * @file test_LocationDirectory_RouteValidation.cpp
 * @brief Corrupt published-root validation coverage for logical location directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t TEST_DATABASE_ID = 0xA0B0C0D0E0F00123ULL;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock(uint64_t generation = 1U, uint64_t lsn = 1U) {
		Superblock result;
		result.database_id = DatabaseId(TEST_DATABASE_ID);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(lsn);
		return result;
	}

	LocationLeafPage leaf(MetadataDirectoryType type = MetadataDirectoryType::MDT_PAGE) {
		LocationLeafPage result;
		result.directory_type = type;
		result.page_id = MetadataPageId(4U);
		result.generation = Generation(2U);
		result.committed_lsn = LogSequenceNumber(3U);
		if (type == MetadataDirectoryType::MDT_PAGE) {
			result.entries = {{1U, SegmentId(10U), Generation(7U), 1U, 1U, 1024U},
							  {126U, SegmentId(11U), Generation(8U), 31U, 1U, 2048U}};
		} else {
			result.entries = {{1U, SegmentId(20U), Generation(4U), 1U, 2U},
							  {126U, SegmentId(21U), Generation(5U), 8U, 24U}};
		}
		return result;
	}

	PageLocation page(uint64_t pageId, uint64_t segmentId, uint16_t pageIndex, uint64_t generation = 1U,
					  uint32_t freeBytes = 0U) {
		return {PageId(pageId), SegmentId(segmentId), pageIndex, Generation(generation), freeBytes};
	}

} // namespace

class RelocatableLocationDirectoryRouteValidationTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<StorageIO> dataIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath = std::filesystem::temp_directory_path() /
				   ("relocatable_location_directory_route_validation_" + to_string(uuid) + ".dat");
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		dataIo = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		dataIo.reset();
		if (dataFile && dataFile->is_open()) {
			dataFile->close();
		}
		dataFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
	}
};

TEST_F(RelocatableLocationDirectoryRouteValidationTest, RejectsMalformedPublishedRootsAndRoutes) {
	const Superblock initial = superblock();
	SuperblockFile(dataIo).initializeNew(initial);
	MetadataPageFile pages(dataIo, initial);
	MetadataPagePoolAllocator allocator(dataIo, initial);
	LocationDirectory directory(pages, allocator);
	LocationLeafPage validLeaf = leaf();
	validLeaf.generation = Generation(1);
	validLeaf.committed_lsn = LogSequenceNumber(1);

	auto writeRoot = [&](MetadataDirectoryType type, CatalogRadixPageKind kind, uint16_t level, PhysicalPageRef child,
						 uint32_t childIndex, LogSequenceNumber lsn = LogSequenceNumber(1)) {
		const PhysicalPageRef rootReference = allocator.allocatePage(Generation(1));
		CatalogRadixPage root;
		root.kind = kind;
		root.directory_type = type;
		root.level = level;
		root.page_id = allocator.allocateMetadataPageId();
		root.generation = Generation(1);
		root.committed_lsn = lsn;
		root.children[childIndex] = child;
		pages.writeMetadataRadix(rootReference, root);
		Superblock result = initial;
		result.page_directory_root = rootReference;
		return result;
	};

	const PhysicalPageRef validLeafReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(validLeafReference, validLeaf);
	MetadataWriteBatch emptyBatch(allocator, initial, LogSequenceNumber(2));
	expectFormatError([&] { directory.prepareUpsertPages(emptyBatch, std::span<const PageLocation>{}); },
					  FormatError::FERR_INVALID_STATE);
	MetadataPagePoolAllocator foreignAllocator(dataIo, initial);
	MetadataWriteBatch foreignBatch(foreignAllocator, initial, LogSequenceNumber(2));
	const PageLocation foreignLocation = page(1, 10, 1);
	expectFormatError([&] { directory.prepareUpsertPages(foreignBatch, std::span(&foreignLocation, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.findPage(initial, PageId{})); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_EXTENT, CatalogRadixPageKind::CRPK_ROOT,
													 0, validLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE,
													 CatalogRadixPageKind::CRPK_INTERNAL, 0, validLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	LocationLeafPage extentLeaf = leaf(MetadataDirectoryType::MDT_EXTENT);
	extentLeaf.generation = Generation(1);
	extentLeaf.committed_lsn = LogSequenceNumber(1);
	const PhysicalPageRef extentLeafReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(extentLeafReference, extentLeaf);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, extentLeafReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	LocationLeafPage wrongRouteLeaf = validLeaf;
	wrongRouteLeaf.entries.front().logical_id = RSS_LOCATION_LEAF_MAX_ENTRIES + 1U;
	wrongRouteLeaf.entries.back().logical_id = 2U * RSS_LOCATION_LEAF_MAX_ENTRIES;
	const PhysicalPageRef wrongRouteReference = allocator.allocatePage(Generation(1));
	pages.writeLocationLeaf(wrongRouteReference, wrongRouteLeaf);
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, wrongRouteReference, 0),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_HEADER);

	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 0, validLeafReference, 0, LogSequenceNumber(2)),
										   PageId(1)));
			},
			FormatError::FERR_INVALID_STATE);

	const PhysicalPageRef malformedInternalReference = allocator.allocatePage(Generation(1));
	CatalogRadixPage malformedInternal;
	malformedInternal.kind = CatalogRadixPageKind::CRPK_ROOT;
	malformedInternal.directory_type = MetadataDirectoryType::MDT_PAGE;
	malformedInternal.level = 0;
	malformedInternal.page_id = allocator.allocateMetadataPageId();
	malformedInternal.generation = Generation(1);
	malformedInternal.committed_lsn = LogSequenceNumber(1);
	malformedInternal.children[0] = validLeafReference;
	pages.writeMetadataRadix(malformedInternalReference, malformedInternal);
	const uint64_t deepId = 1U + static_cast<uint64_t>(RSS_LOCATION_LEAF_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT;
	expectFormatError(
			[&] {
				static_cast<void>(
						directory.findPage(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT,
													 1, malformedInternalReference, 1),
										   PageId(deepId)));
			},
			FormatError::FERR_INVALID_HEADER);

	auto expectPrepareError = [&](const Superblock &current, PageLocation update, FormatError expected) {
		MetadataWriteBatch batch(allocator, current, LogSequenceNumber(2));
		expectFormatError([&] { directory.prepareUpsertPages(batch, std::span(&update, 1)); }, expected);
	};
	expectPrepareError(
			writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 0, extentLeafReference, 0),
			page(1, 10, 1), FormatError::FERR_INVALID_HEADER);

	const auto writeInternal = [&](CatalogRadixPageKind kind, MetadataDirectoryType type, uint16_t level,
								   Generation generation) {
		const PhysicalPageRef reference = allocator.allocatePage(generation);
		CatalogRadixPage internal;
		internal.kind = kind;
		internal.directory_type = type;
		internal.level = level;
		internal.page_id = allocator.allocateMetadataPageId();
		internal.generation = generation;
		internal.committed_lsn = LogSequenceNumber(1);
		internal.children[0] = validLeafReference;
		pages.writeMetadataRadix(reference, internal);
		return reference;
	};
	for (const PhysicalPageRef invalidInternal: {
				 writeInternal(CatalogRadixPageKind::CRPK_ROOT, MetadataDirectoryType::MDT_PAGE, 0, Generation(1)),
				 writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_EXTENT, 0,
							   Generation(1)),
				 writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_PAGE, 1, Generation(1)),
		 }) {
		expectPrepareError(
				writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 1, invalidInternal, 1),
				page(deepId, 10, 1), FormatError::FERR_INVALID_HEADER);
	}
	const PhysicalPageRef newerInternal =
			writeInternal(CatalogRadixPageKind::CRPK_INTERNAL, MetadataDirectoryType::MDT_PAGE, 0, Generation(2));
	expectPrepareError(writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 1, newerInternal, 1),
					   page(deepId, 10, 1), FormatError::FERR_INVALID_STATE);
	Superblock malformedForRecovery =
			writeRoot(MetadataDirectoryType::MDT_PAGE, CatalogRadixPageKind::CRPK_ROOT, 0, wrongRouteReference, 0);
	expectFormatError([&] { allocator.recover(malformedForRecovery); }, FormatError::FERR_INVALID_HEADER);
}
