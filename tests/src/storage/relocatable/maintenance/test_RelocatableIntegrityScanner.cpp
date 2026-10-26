/**
 * @file test_RelocatableIntegrityScanner.cpp
 * @brief Cross-directory integrity tests for the read-only relocatable scanner.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "storage/relocatable/TestSlottedPageRecord.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;
using graph::storage::test::TestSlottedPageRecord;
using graph::storage::test::TestSlottedPageRecordCodec;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	OrderedBTreeEntrySource orderedSource(std::vector<BTreePageEntry> entries) {
		const auto shared = std::make_shared<const std::vector<BTreePageEntry>>(std::move(entries));
		return [shared] {
			auto position = std::make_shared<size_t>(0U);
			return [shared, position]() -> std::optional<BTreePageEntry> {
				if (*position == shared->size()) {
					return std::nullopt;
				}
				return (*shared)[(*position)++];
			};
		};
	}

	VectorManifest manifest() {
		VectorManifest result;
		result.index_id = 17;
		result.page_id = PageId(102);
		result.generation = Generation(1);
		result.committed_lsn = LogSequenceNumber(6);
		result.dimensions = 2;
		return result;
	}

	PageOwnerHeader manifestOwner(const VectorManifest &manifest) {
		return {manifest.page_id,
				manifest.index_id,
				PageOwnerKind::POK_VECTOR_MANIFEST,
				0,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				manifest.generation,
				manifest.committed_lsn};
	}

	SharedPagePoolAllocationRequest manifestRequest(const VectorManifest &manifest) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(73);
		result.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		result.pool_owner_id = manifest.index_id;
		result.generation = manifest.generation;
		result.page_id = manifest.page_id;
		result.page_owner_id = manifest.index_id;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		return result;
	}

	uint64_t pagePayloadOffset(RelocatableMetadataStore &metadata, PageId pageId) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
														   pool.physical_slot) +
			   static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
	}

	uint64_t extentHeaderOffset(RelocatableMetadataStore &metadata, SegmentId segmentId) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const CatalogEntry entry = metadata.findCatalog(snapshot, segmentId);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
														   entry.physical_slot) +
			   RSS_ENVELOPE_BYTES;
	}

	void overwritePagePayload(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							  std::span<const std::byte> bytes) {
		io->writeAt(pagePayloadOffset(metadata, pageId), bytes.data(), bytes.size());
	}

	template<size_t Size>
	void overwriteExtentHeader(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
							   SegmentId segmentId, const std::array<std::byte, Size> &bytes) {
		io->writeAt(extentHeaderOffset(metadata, segmentId), bytes.data(), bytes.size());
	}

	LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const LogSequenceNumber result(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		return result;
	}

	LargeValueWriteOptions largeValueWriteOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(90U);
		result.first_manifest_slot = PhysicalSlotId(91U);
		result.last_manifest_slot = PhysicalSlotId(100U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = "application/octet-stream";
		result.extent_class = LargeValueExtentClass::LVEC_16K;
		return result;
	}

	std::vector<std::byte> largeValueBytes(size_t count) {
		std::vector<std::byte> result(count);
		for (size_t ordinal = 0U; ordinal < result.size(); ++ordinal) {
			result[ordinal] = static_cast<std::byte>((ordinal * 29U + 7U) & 0xFFU);
		}
		return result;
	}

	LargeValueWriteResult writeMultiLevelLargeValue(const std::shared_ptr<StorageIO> &io,
													RelocatableMetadataStore &metadata) {
		LargeValueStore values(io, metadata);
		const size_t bytes =
				(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES +
				19U;
		return values.write(largeValueBytes(bytes), largeValueWriteOptions(), nextCheckpointLsn(metadata));
	}

	void overwriteLargeValueManifest(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
									 const LargeValueManifest &manifest) {
		overwritePagePayload(io, metadata, manifest.page_id, LargeValueManifestPageCodec::encode(manifest));
	}

	void overwriteLargeValueManifestAtPage(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
										   PageId physicalPageId, const LargeValueManifest &manifest) {
		overwritePagePayload(io, metadata, physicalPageId, LargeValueManifestPageCodec::encode(manifest));
	}

	struct LargeValueRouteLeafImage {
		Superblock superblock;
		PhysicalPageRef leaf_reference;
		LocationLeafPage leaf;
		size_t entry_index = 0U;
	};

	LargeValueRouteLeafImage readLargeValueRouteLeaf(const std::shared_ptr<StorageIO> &io,
													RelocatableMetadataStore &metadata, LargeValueId valueId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		LargeValueRouteLeafImage result;
		result.superblock = snapshot.superblock();
		MetadataPageFile pages(io, result.superblock);
		const uint64_t leafOrdinal = (valueId.value() - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES;
		MetadataRadixPage radix = pages.readMetadataRadix(result.superblock.large_value_manifest_directory_root);
		for (;;) {
			const std::optional<PhysicalPageRef> child =
					CatalogRadixPageCodec::findChildForLeafOrdinal(radix, leafOrdinal);
			if (!child.has_value()) {
				throw std::runtime_error("test LargeValue Manifest directory is missing its route leaf");
			}
			if (radix.level == 0U) {
				result.leaf_reference = *child;
				break;
			}
			radix = pages.readMetadataRadix(*child);
		}
		result.leaf = pages.readLocationLeaf(result.leaf_reference);
		const auto entry = std::find_if(result.leaf.entries.begin(), result.leaf.entries.end(),
									 [valueId](const LocationLeafEntry &candidate) {
										 return candidate.logical_id == valueId.value();
									 });
		if (entry == result.leaf.entries.end()) {
			throw std::runtime_error("test LargeValue Manifest directory leaf is missing its route");
		}
		result.entry_index = static_cast<size_t>(std::distance(result.leaf.entries.begin(), entry));
		return result;
	}

	void overwriteLargeValueRouteLeaf(const std::shared_ptr<StorageIO> &io, const LargeValueRouteLeafImage &image) {
		MetadataPageFile(io, image.superblock).writeLocationLeaf(image.leaf_reference, image.leaf);
	}

	SharedPagePoolContents readLargeValuePage(const std::shared_ptr<StorageIO> &io,
											RelocatableMetadataStore &metadata, PageId pageId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		return SharedPagePoolAllocator(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY).read(snapshot, pageId);
	}

	LargeValueManifest readLargeValueManifest(const std::shared_ptr<StorageIO> &io,
											 RelocatableMetadataStore &metadata, PageId pageId) {
		const SharedPagePoolContents contents = readLargeValuePage(io, metadata, pageId);
		return LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	}

	LargeValueDataPage readLargeValueDataPage(const std::shared_ptr<StorageIO> &io,
											 RelocatableMetadataStore &metadata, PageId pageId) {
		const SharedPagePoolContents contents = readLargeValuePage(io, metadata, pageId);
		return LargeValueDataFrame::open(contents.owner, contents.payload).page();
	}

	void overwriteLargeValueDataPage(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
									 PageId physicalPageId, const LargeValueDataPage &page) {
		overwritePagePayload(io, metadata, physicalPageId, LargeValueDataPageCodec::encode(page));
	}

	void allocateOrphanLargeValueManifest(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
										  const LargeValueManifest &source) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const Generation poolGeneration = snapshot.superblock().generation;
		snapshot.release();
		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata);
		LargeValueManifest orphan = source;
		orphan.page_id = PageId(900001U);
		const PageOwnerHeader owner{orphan.page_id,
									source.value_id.value(),
									PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
									0U,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									poolGeneration,
									checkpointLsn};
		SharedPagePoolAllocationRequest request;
		request.segment_id = SegmentId(900001U);
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = orphan.page_id;
		request.page_owner_id = source.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
		request.payload = LargeValueManifestPage::create(owner, orphan).serialize();
		SharedPagePoolAllocator pools(io, metadata);
		ASSERT_TRUE(pools.allocateNewPoolPage(request, PhysicalSlotId(101U), PhysicalSlotId(101U), checkpointLsn)
							.has_value());
	}

	void allocateOrphanLargeValueDataPage(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
										  const LargeValueRef &reference) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const Generation poolGeneration = snapshot.superblock().generation;
		snapshot.release();
		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata);
		const PageId pageId(900002U);
		const LargeValueDataPage orphan{reference.value_id, pageId, reference.generation, 0U, 0U, 1U, 1U, 0U,
										{std::byte{0xA5U}}};
		const PageOwnerHeader owner{pageId,		  reference.value_id.value(),		PageOwnerKind::POK_LARGE_VALUE_DATA,
									0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, poolGeneration,
									checkpointLsn};
		SharedPagePoolAllocationRequest request;
		request.segment_id = SegmentId(900002U);
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = reference.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
		request.payload = LargeValueDataFrame::create(owner, orphan).serialize();
		SharedPagePoolAllocator pools(io, metadata);
		ASSERT_TRUE(pools.allocateNewPoolPage(request, PhysicalSlotId(102U), PhysicalSlotId(102U), checkpointLsn)
							.has_value());
	}

} // namespace

class RelocatableIntegrityScannerTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_integrity_scanner_" + to_string(uuid));
		walPath = path.string() + ".foreign";
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (file && file->is_open()) {
			file->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatableIntegrityScannerTest, RejectsNullOrForeignDataStorage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(nullptr, *metadata)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(walIo, *metadata)), std::invalid_argument);
}

TEST_F(RelocatableIntegrityScannerTest, ScansEveryPageOfAMultiLevelLargeValueManifestTree) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_EQ(written.manifest.node_kind, LargeValueManifestNodeKind::LVMNK_ROOT);
	ASSERT_EQ(written.manifest.node_level, 1U);
	ASSERT_EQ(written.manifest.children.size(), 2U);

	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io, *metadata).scan();
	EXPECT_EQ(report.large_value_manifest_routes, 1U);
	EXPECT_EQ(report.large_value_manifest_nodes, 3U);
	EXPECT_GT(report.large_value_data_pages, 0U);
	EXPECT_EQ(report.large_value_content_index_entries, 1U);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueContentIndexValueThatDisagreesWithItsManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	static_cast<void>(writeMultiLevelLargeValue(io, *metadata));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	SharedPagePoolAllocator pageReader(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = pageReader.read(snapshot, root.root_page_id);
	const BTreePage original = BTreePage::open(contents.owner, contents.payload);
	ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
	ASSERT_EQ(original.entries().size(), 1U);
	BTreePageHeader corruptHeader = original.header();
	corruptHeader.entry_count = 0U;
	BTreePage corrupt = BTreePage::create(contents.owner, corruptHeader);
	BTreePageEntry entry = original.entries().front();
	ASSERT_LT(entry.value_or_child, std::numeric_limits<uint64_t>::max());
	++entry.value_or_child;
	corrupt.insert(std::move(entry));
	snapshot.release();
	overwritePagePayload(io, *metadata, root.root_page_id, corrupt.serialize());

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsIncompleteOrOrphanLargeValueContentIndex) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	static_cast<void>(writeMultiLevelLargeValue(io, *metadata));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	SharedPagePoolAllocator pageReader(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = pageReader.read(snapshot, root.root_page_id);
	const BTreePage original = BTreePage::open(contents.owner, contents.payload);
	const std::vector<BTreePageEntry> originalEntries = original.entries();
	ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
	ASSERT_EQ(originalEntries.size(), 1U);
	snapshot.release();

	const auto replaceEntries = [&](std::span<const BTreePageEntry> entries) {
		BTreePageHeader header = original.header();
		header.entry_count = 0U;
		BTreePage replacement = BTreePage::create(contents.owner, header);
		for (const BTreePageEntry &entry: entries) {
			replacement.insert(entry);
		}
		MetadataSnapshotPin current = metadata->openSnapshot();
		PageLocation location = metadata->findPageLocation(current, root.root_page_id);
		current.release();
		location.free_bytes = replacement.slottedPage().stats().contiguous_free_bytes;
		overwritePagePayload(io, *metadata, root.root_page_id, replacement.serialize());
		static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	};
	RelocatableIntegrityScanner scanner(io, *metadata);

	const std::array<BTreePageEntry, 0U> empty{};
	replaceEntries(empty);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	BTreePageEntry unknownKey = originalEntries.front();
	unknownKey.key.front() ^= std::byte{0x7FU};
	const std::array unknownEntries{unknownKey};
	replaceEntries(unknownEntries);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	BTreePageEntry extra = originalEntries.front();
	extra.key.front() ^= std::byte{0x3FU};
	extra.value_or_child += 1U;
	const std::array extraEntries{originalEntries.front(), extra};
	replaceEntries(extraEntries);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	replaceEntries(originalEntries);
	const std::array<uint64_t, 1U> erasedRoot{RSS_LARGE_VALUE_CONTENT_INDEX_ID};
	SlottedPagePool indexPages(io, *metadata);
	BTreePageStore indexTrees(indexPages, *metadata);
	static_cast<void>(indexTrees.publishRoots({}, erasedRoot, nextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueManifestCycle) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_EQ(written.manifest.children.size(), 2U);
	LargeValueManifest corrupt = written.manifest;
	corrupt.children[1].page_id = corrupt.children[0].page_id;
	overwriteLargeValueManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueManifestChildWithMismatchedSubtreeRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_EQ(written.manifest.children.size(), 2U);
	LargeValueManifest corrupt = written.manifest;
	ASSERT_LT(corrupt.children[0].chunk_count, std::numeric_limits<uint32_t>::max());
	ASSERT_LT(corrupt.children[1].first_chunk_ordinal, std::numeric_limits<uint32_t>::max());
	++corrupt.children[0].chunk_count;
	++corrupt.children[1].first_chunk_ordinal;
	overwriteLargeValueManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueRootManifestThatDisagreesWithItsDirectoryRoute) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	LargeValueManifest corrupt = written.manifest;
	corrupt.page_id = PageId(900003U);
	overwriteLargeValueManifestAtPage(io, *metadata, written.manifest.page_id, corrupt);

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsOrphanLargeValueManifestPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	allocateOrphanLargeValueManifest(io, *metadata, written.manifest);
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsOrphanLargeValueDataPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	allocateOrphanLargeValueDataPage(io, *metadata, written.reference);
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueManifestDirectoryRelationshipCorruption) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_FALSE(written.manifest.children.empty());
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const LargeValueRouteLeafImage original = readLargeValueRouteLeaf(io, *metadata, written.reference.value_id);
	const auto reject = [&](const LocationLeafEntry &entry) {
		LargeValueRouteLeafImage corrupt = original;
		corrupt.leaf.entries[corrupt.entry_index] = entry;
		overwriteLargeValueRouteLeaf(io, corrupt);
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		overwriteLargeValueRouteLeaf(io, original);
	};

	LocationLeafEntry entry = original.leaf.entries[original.entry_index];
	entry.generation = Generation(entry.generation.value() + 1U);
	reject(entry);
	entry = original.leaf.entries[original.entry_index];
	++entry.logical_id;
	reject(entry);
	entry = original.leaf.entries[original.entry_index];
	entry.segment_id = SegmentId(900003U);
	reject(entry);
	entry = original.leaf.entries[original.entry_index];
	entry.segment_id = SegmentId(written.manifest.children.front().page_id.value());
	reject(entry);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
}

TEST_F(RelocatableIntegrityScannerTest, RejectsStaleLargeValuePageFreeSpaceMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_FALSE(written.manifest.children.empty());
	const LargeValueManifest leaf = readLargeValueManifest(io, *metadata, written.manifest.children.front().page_id);
	ASSERT_FALSE(leaf.chunks.empty());
	RelocatableIntegrityScanner scanner(io, *metadata);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	PageLocation location = metadata->findPageLocation(snapshot, written.manifest.page_id);
	snapshot.release();
	const PageLocation manifestLocation = location;
	location.free_bytes = 1U;
	static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	location = manifestLocation;
	static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));

	snapshot = metadata->openSnapshot();
	location = metadata->findPageLocation(snapshot, leaf.chunks.front().first_data_page_id);
	snapshot.release();
	const PageLocation dataLocation = location;
	location.free_bytes = 1U;
	static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	location = dataLocation;
	static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
}

TEST_F(RelocatableIntegrityScannerTest, RejectsLargeValueTreeAndDataRelationshipCorruption) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LargeValueWriteResult written = writeMultiLevelLargeValue(io, *metadata);
	ASSERT_GE(written.manifest.children.size(), 2U);
	const PageId leafPageId = written.manifest.children.front().page_id;
	const LargeValueManifest originalLeaf = readLargeValueManifest(io, *metadata, leafPageId);
	ASSERT_GE(originalLeaf.chunks.size(), 2U);
	ASSERT_GT(originalLeaf.chunks.front().data_page_count, 1U);
	RelocatableIntegrityScanner scanner(io, *metadata);
	const auto rejectManifest = [&](PageId pageId, const LargeValueManifest &corrupt,
									  const LargeValueManifest &original) {
		overwriteLargeValueManifestAtPage(io, *metadata, pageId, corrupt);
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		overwriteLargeValueManifestAtPage(io, *metadata, pageId, original);
	};

	LargeValueManifest corruptLeaf = originalLeaf;
	corruptLeaf.chunks.clear();
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	LargeValueManifest corruptRoot = written.manifest;
	corruptRoot.children.front().page_id = PageId(900004U);
	rejectManifest(written.manifest.page_id, corruptRoot, written.manifest);
	corruptLeaf = originalLeaf;
	corruptLeaf.generation = Generation(corruptLeaf.generation.value() + 1U);
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	corruptRoot = written.manifest;
	++corruptRoot.node_level;
	rejectManifest(written.manifest.page_id, corruptRoot, written.manifest);
	corruptLeaf = originalLeaf;
	++corruptLeaf.first_chunk_ordinal;
	for (LargeValueChunk &chunk: corruptLeaf.chunks) {
		++chunk.ordinal;
	}
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	corruptLeaf = originalLeaf;
	++corruptLeaf.first_uncompressed_offset;
	for (LargeValueChunk &chunk: corruptLeaf.chunks) {
		++chunk.uncompressed_offset;
	}
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	corruptLeaf = originalLeaf;
	++corruptLeaf.node_logical_bytes;
	++corruptLeaf.chunks.back().uncompressed_bytes;
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	corruptRoot = written.manifest;
	++corruptRoot.children[0].logical_bytes;
	++corruptRoot.children[1].uncompressed_offset;
	--corruptRoot.children[1].logical_bytes;
	rejectManifest(written.manifest.page_id, corruptRoot, written.manifest);

	corruptLeaf = originalLeaf;
	corruptLeaf.chunks.front().first_data_page_id = PageId(900005U);
	rejectManifest(leafPageId, corruptLeaf, originalLeaf);
	const PageId dataPageId = originalLeaf.chunks.front().first_data_page_id;
	const LargeValueDataPage originalData = readLargeValueDataPage(io, *metadata, dataPageId);
	const auto rejectData = [&](const LargeValueDataPage &corrupt) {
		overwriteLargeValueDataPage(io, *metadata, dataPageId, corrupt);
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		overwriteLargeValueDataPage(io, *metadata, dataPageId, originalData);
	};
	LargeValueDataPage corruptData = originalData;
	corruptData.generation = Generation(corruptData.generation.value() + 1U);
	rejectData(corruptData);
	corruptData = originalData;
	++corruptData.chunk_ordinal;
	rejectData(corruptData);
	corruptData = originalData;
	corruptData.page_ordinal = 1U;
	corruptData.stored_offset = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
	corruptData.payload.assign(
			std::min<uint32_t>(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES,
							   corruptData.stored_chunk_bytes - corruptData.stored_offset),
			std::byte{0x5AU});
	rejectData(corruptData);
	corruptData = originalData;
	++corruptData.stored_chunk_bytes;
	rejectData(corruptData);
	corruptData = originalData;
	corruptData.stored_chunk_bytes += RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
	++corruptData.page_count;
	rejectData(corruptData);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
}

TEST_F(RelocatableIntegrityScannerTest, RejectsPageDirectoryEntriesWithoutACatalogSegment) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const PageLocation orphan{PageId(101), SegmentId(77), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1), 0};
	static_cast<void>(metadata->upsertLocations(std::span(&orphan, 1), {}, LogSequenceNumber(2)));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsPageDirectoryEntriesOutsideSharedPools) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator physical(io, *metadata);
	PhysicalSegmentAllocationRequest nonPagePool;
	nonPagePool.segment_id = SegmentId(77);
	nonPagePool.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	nonPagePool.owner_id = 100U;
	nonPagePool.generation = Generation(1);
	ASSERT_TRUE(physical.allocate(nonPagePool, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const PageLocation invalid{PageId(101), SegmentId(77), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1), 0};
	static_cast<void>(metadata->upsertLocations(std::span(&invalid, 1), {}, LogSequenceNumber(3)));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsMalformedOrUnsupportedExtentDirectoryEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator physical(io, *metadata);
	PhysicalSegmentAllocationRequest entityPool;
	entityPool.segment_id = SegmentId(77);
	entityPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	entityPool.owner_id = 100U;
	entityPool.generation = Generation(1);
	ASSERT_TRUE(physical.allocate(entityPool, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	ExtentLocation malformed{ExtentId(1), SegmentId(77), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1, Generation(1)};
	static_cast<void>(metadata->upsertLocations({}, std::span(&malformed, 1), LogSequenceNumber(3)));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	ExtentLocation unsupported{ExtentId(1), SegmentId(77), RSS_PAGE_POOL_FIRST_DATA_FRAME,
							   RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	static_cast<void>(metadata->upsertLocations({}, std::span(&unsupported, 1), LogSequenceNumber(4)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, ScansStateDirectoryRecordsAndRejectsStaleLocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord record{
			11, "system.config", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x31}, std::byte{0x32}}};
	const StateRecordStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(40), 800};
	const StateLocation location = states.upsert(record, options);
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_EQ(scanner.scan().state_locations, 1U);

	const StateLocation stale{location.state_id, location.page_id, location.slot_id, location.slot_generation + 1U};
	static_cast<void>(metadata->upsertStateLocations(std::span(&stale, 1), {}, nextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsStateDirectoryIdsWithoutMatchingRecords) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord record{15, "system.audit", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x61}}};
	const StateRecordStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(40), 804};
	const StateLocation location = states.upsert(record, options);
	const StateLocation absent{99, location.page_id, location.slot_id, location.slot_generation};
	static_cast<void>(metadata->upsertStateLocations(std::span(&absent, 1), {}, nextCheckpointLsn(*metadata)));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsStatePagesWithStaleFreeSpaceOrRecordsWithoutDirectoryEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord record{13, "system.cache", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x41}}};
	const StateRecordStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(40), 802};
	const StateLocation location = states.upsert(record, options);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation current = metadata->findPageLocation(snapshot, location.page_id);
	snapshot.release();
	PageLocation staleFreeSpace = current;
	++staleFreeSpace.free_bytes;
	static_cast<void>(metadata->upsertLocations(std::span(&staleFreeSpace, 1), {}, nextCheckpointLsn(*metadata)));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(metadata->upsertLocations(std::span(&current, 1), {}, nextCheckpointLsn(*metadata)));
	const std::array erased = {location.state_id};
	static_cast<void>(metadata->upsertStateLocations({}, erased, nextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsStateReferencesToMissingLargeValueManifests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord external{12, "system.payload", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(77), {}};
	const StateRecordStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(40), 801};
	static_cast<void>(states.upsert(external, options));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, ScansStateReferencesToExistingLargeValueManifests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *metadata);
	LargeValueWriteOptions writeOptions;
	writeOptions.first_data_slot = PhysicalSlotId(40);
	writeOptions.last_data_slot = PhysicalSlotId(80);
	writeOptions.first_manifest_slot = PhysicalSlotId(81);
	writeOptions.last_manifest_slot = PhysicalSlotId(100);
	writeOptions.kind = LargeValueKind::LVK_BYTES;
	writeOptions.media_type = std::string(RSS_STATE_LARGE_VALUE_MEDIA_TYPE);
	const LargeValueWriteResult written =
			values.write(std::array{std::byte{0x51}, std::byte{0x52}}, writeOptions, LogSequenceNumber(2));
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord external{14, "system.backup", StateRecordStorageKind::SRS_LARGE_VALUE, written.reference.value_id, {}};
	const StateRecordStoreOptions options{PhysicalSlotId(101), PhysicalSlotId(101), 803};
	static_cast<void>(states.upsert(external, options));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_EQ(scanner.scan().state_locations, 1U);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsStateReferencesToLargeValuesWithAnotherOwnerMediaType) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *metadata);
	LargeValueWriteOptions writeOptions;
	writeOptions.first_data_slot = PhysicalSlotId(40);
	writeOptions.last_data_slot = PhysicalSlotId(80);
	writeOptions.first_manifest_slot = PhysicalSlotId(81);
	writeOptions.last_manifest_slot = PhysicalSlotId(100);
	writeOptions.kind = LargeValueKind::LVK_BYTES;
	writeOptions.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
	const LargeValueWriteResult written = values.write(std::array{std::byte{0x51}}, writeOptions, LogSequenceNumber(2));
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	static_cast<void>(states.upsert(
			{14, "system.wrong-namespace", StateRecordStorageKind::SRS_LARGE_VALUE, written.reference.value_id, {}},
			{PhysicalSlotId(101), PhysicalSlotId(101), 803}));
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsEntityPropertyRootsWithoutTheirNativeEntityOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 100U};
	const PropertyCollectionRoot root{
			key.entity_id, graph::Node::typeId, {{3U, graph::PropertyValue("orphan native property root")}}};
	const PageOwnerHeader owner{PageId(101U),
									  901U,
									  PageOwnerKind::POK_PROPERTY,
									  0U,
									  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									  Generation(1U),
									  LogSequenceNumber(2U)};
	SlottedPage page = SlottedPage::create(owner);
	const SlottedSlotReference slot = page.insert(EntityPropertyCollectionRootCodec::encode(root));
	SlottedPagePoolAllocationRequest allocation;
	allocation.segment_id = SegmentId(71U);
	allocation.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	allocation.pool_owner_id = owner.owner_id;
	allocation.generation = owner.generation;
	allocation.page_id = owner.page_id;
	allocation.page_owner_id = owner.owner_id;
	allocation.page_owner_kind = owner.kind;
	ASSERT_TRUE(pages.allocatePage(allocation, page, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	const EntityPropertyLocation location{key, slot.page_id, slot.slot_id, slot.generation};
	static_cast<void>(metadata->upsertEntityPropertyLocations(std::span(&location, 1U), {}, LogSequenceNumber(3U)));
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, ScansAndValidatesNodeToVectorMappingRoots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore indexes(pages, *metadata);
	const uint64_t indexId = 29;
	SlottedPagePoolAllocationRequest indexRequest;
	indexRequest.segment_id = SegmentId(91);
	indexRequest.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	indexRequest.pool_owner_id = indexId;
	indexRequest.generation = Generation(1);
	indexRequest.page_id = PageId(501);
	indexRequest.page_owner_id = indexId;
	indexRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	const PageOwnerHeader indexOwner{indexRequest.page_id,
									 indexRequest.page_owner_id,
									 indexRequest.page_owner_kind,
									 0,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 indexRequest.generation,
									 LogSequenceNumber(2)};
	BTreePageHeader indexHeader;
	indexHeader.index_id = indexId;
	indexHeader.page_id = indexRequest.page_id;
	indexHeader.generation = indexRequest.generation;
	BTreePage indexPage = BTreePage::create(indexOwner, indexHeader);
	indexPage.insert({VectorSemanticCodec::nodeMappingKey(101U), 701U});
	ASSERT_TRUE(indexes.allocate(indexRequest, indexPage, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{indexId, indexHeader.page_id, indexHeader.generation};
	static_cast<void>(indexes.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	const uint64_t vectorNodeMappingIndexId = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x29);
	SlottedPagePoolAllocationRequest vectorNodeMappingRequest = indexRequest;
	vectorNodeMappingRequest.segment_id = SegmentId(93);
	vectorNodeMappingRequest.pool_owner_id = vectorNodeMappingIndexId;
	vectorNodeMappingRequest.page_id = PageId(504);
	vectorNodeMappingRequest.page_owner_id = vectorNodeMappingIndexId;
	const PageOwnerHeader vectorNodeMappingOwner{vectorNodeMappingRequest.page_id,
												 vectorNodeMappingRequest.page_owner_id,
												 vectorNodeMappingRequest.page_owner_kind,
												 0,
												 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												 vectorNodeMappingRequest.generation,
												 LogSequenceNumber(4)};
	BTreePageHeader vectorNodeMappingHeader;
	vectorNodeMappingHeader.index_id = vectorNodeMappingIndexId;
	vectorNodeMappingHeader.page_id = vectorNodeMappingRequest.page_id;
	vectorNodeMappingHeader.generation = vectorNodeMappingRequest.generation;
	BTreePage vectorNodeMappingPage = BTreePage::create(vectorNodeMappingOwner, vectorNodeMappingHeader);
	vectorNodeMappingPage.insert({VectorNodeMapping::key(701U), 101U});
	ASSERT_TRUE(indexes.allocate(vectorNodeMappingRequest, vectorNodeMappingPage, PhysicalSlotId(41),
								 PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());
	const IndexRootLocation vectorNodeMappingRoot{vectorNodeMappingIndexId, vectorNodeMappingRequest.page_id,
												  vectorNodeMappingRequest.generation};
	static_cast<void>(indexes.publishRoots(std::span(&vectorNodeMappingRoot, 1), {}, LogSequenceNumber(5)));

	VectorPageHeader rawHeader;
	rawHeader.index_id = indexId;
	rawHeader.page_id = PageId(505U);
	rawHeader.generation = Generation(1U);
	rawHeader.kind = VectorStorageKind::VSK_RAW;
	rawHeader.record_bytes = sizeof(uint64_t) + 2U * sizeof(uint16_t);
	rawHeader.slot_capacity = 3U;
	rawHeader.bitmap_bytes = 1U;
	rawHeader.committed_lsn = LogSequenceNumber(6U);
	const PageOwnerHeader rawOwner{rawHeader.page_id,
								   rawHeader.index_id,
								   PageOwnerKind::POK_VECTOR_RAW,
								   0U,
								   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								   rawHeader.generation,
								   rawHeader.committed_lsn};
	VectorPage rawPage = VectorPage::create(rawOwner, rawHeader);
	std::vector<std::byte> rawRecord(rawHeader.record_bytes);
	LittleEndianCodec::writeU64(rawRecord, 0U, 701U);
	const uint16_t rawSlot = rawPage.insert(rawRecord);
	VectorPageStore vectors(io, *metadata);
	SharedPagePoolAllocationRequest rawRequest;
	rawRequest.segment_id = SegmentId(94U);
	rawRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawRequest.pool_owner_id = indexId;
	rawRequest.generation = rawHeader.generation;
	rawRequest.page_id = rawHeader.page_id;
	rawRequest.page_owner_id = indexId;
	rawRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(vectors.allocate(rawRequest, rawPage, PhysicalSlotId(43U), PhysicalSlotId(43U), LogSequenceNumber(6U))
						.has_value());
	const VectorLocation rawLocation{701U, rawHeader.page_id, rawSlot, rawPage.slotGeneration(rawSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&rawLocation, 1U), {}, LogSequenceNumber(7U)));

	VectorManifest manifest;
	manifest.index_id = indexId;
	manifest.page_id = PageId(502);
	manifest.generation = Generation(1);
	manifest.committed_lsn = LogSequenceNumber(8U);
	manifest.dimensions = 2;
	manifest.node_mapping_root_page_id = root.root_page_id;
	manifest.node_mapping_root_generation = root.root_generation;
	manifest.vector_node_mapping_index_id = vectorNodeMappingRoot.index_id;
	manifest.vector_node_mapping_root_page_id = vectorNodeMappingRoot.root_page_id;
	manifest.vector_node_mapping_root_generation = vectorNodeMappingRoot.root_generation;
	const PageOwnerHeader manifestOwner{manifest.page_id,
										manifest.index_id,
										PageOwnerKind::POK_VECTOR_MANIFEST,
										0,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										manifest.generation,
										manifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = SegmentId(92);
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = manifest.index_id;
	manifestRequest.generation = manifest.generation;
	manifestRequest.page_id = manifest.page_id;
	manifestRequest.page_owner_id = manifest.index_id;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestOwner, manifest),
								  PhysicalSlotId(44U), PhysicalSlotId(44U), manifest.committed_lsn)
						.has_value());

	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	VectorPage mismatchedRaw = VectorPage::create(rawOwner, rawHeader);
	std::vector<std::byte> mismatchedRawRecord(rawHeader.record_bytes);
	LittleEndianCodec::writeU64(mismatchedRawRecord, 0U, 702U);
	static_cast<void>(mismatchedRaw.insert(mismatchedRawRecord));
	overwritePagePayload(io, *metadata, rawHeader.page_id, mismatchedRaw.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, rawHeader.page_id, rawPage.serialize());
	VectorManifest corrupt = manifest;
	corrupt.node_mapping_root_page_id = PageId(503);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, corrupt).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, manifest).serialize());
	corrupt = manifest;
	corrupt.node_mapping_root_generation = Generation(2);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, corrupt).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, manifest).serialize());
	corrupt = manifest;
	corrupt.vector_node_mapping_root_page_id = PageId(503U);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, corrupt).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, manifest).serialize());
	corrupt = manifest;
	corrupt.raw_write_tail_page_id = PageId(503U);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, corrupt).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, manifest.page_id,
						 VectorManifestPage::create(manifestOwner, manifest).serialize());
	BTreePage incompleteNodeMapping = BTreePage::create(indexOwner, indexHeader);
	overwritePagePayload(io, *metadata, indexRequest.page_id, incompleteNodeMapping.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, indexRequest.page_id, indexPage.serialize());
	BTreePage nonBijectiveNodeMapping = BTreePage::create(indexOwner, indexHeader);
	nonBijectiveNodeMapping.insert({VectorSemanticCodec::nodeMappingKey(101U), 0U});
	overwritePagePayload(io, *metadata, indexRequest.page_id, nonBijectiveNodeMapping.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, indexRequest.page_id, indexPage.serialize());
	BTreePage incompleteReverseMapping = BTreePage::create(vectorNodeMappingOwner, vectorNodeMappingHeader);
	overwritePagePayload(io, *metadata, vectorNodeMappingRequest.page_id, incompleteReverseMapping.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, vectorNodeMappingRequest.page_id, vectorNodeMappingPage.serialize());
	BTreePage zeroReverseNode = BTreePage::create(vectorNodeMappingOwner, vectorNodeMappingHeader);
	zeroReverseNode.insert({VectorNodeMapping::key(701U), 0U});
	overwritePagePayload(io, *metadata, vectorNodeMappingRequest.page_id, zeroReverseNode.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwritePagePayload(io, *metadata, vectorNodeMappingRequest.page_id, vectorNodeMappingPage.serialize());
	BTreePage mismatchedReverse = BTreePage::create(vectorNodeMappingOwner, vectorNodeMappingHeader);
	mismatchedReverse.insert({VectorNodeMapping::key(701U), 102U});
	overwritePagePayload(io, *metadata, vectorNodeMappingRequest.page_id, mismatchedReverse.serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, ScansMultiLevelBidirectionalVectorMappingTrees) {
	constexpr uint64_t INDEX_ID = 29U;
	constexpr uint64_t REVERSE_INDEX_ID = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;
	const auto vectorCountRequiringAnInternalRoot = [] {
		constexpr size_t packedEntryCapacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
											   2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
		std::vector<BTreePageEntry> entries;
		for (uint64_t vectorId = 1U;; ++vectorId) {
			entries.push_back({VectorSemanticCodec::nodeMappingKey(vectorId), vectorId});
			if (PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries).size() > packedEntryCapacity) {
				return vectorId;
			}
			if (vectorId == std::numeric_limits<uint64_t>::max()) {
				throw std::runtime_error("B+Tree leaf capacity cannot be exceeded safely");
			}
		}
	};
	const uint64_t vectorCount = vectorCountRequiringAnInternalRoot();
	ASSERT_LE(vectorCount, std::numeric_limits<uint16_t>::max());
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const SegmentId manifestSegmentId = metadata->allocateLogicalSegmentIds(1U, LogSequenceNumber(2U)).front();

	std::vector<RawVectorMaterializationRecord> rawRecords;
	rawRecords.reserve(static_cast<size_t>(vectorCount));
	for (uint64_t vectorId = 1U; vectorId <= vectorCount; ++vectorId) {
		RawVectorMaterializationRecord record;
		record.vector_id = vectorId;
		record.bytes.resize(sizeof(uint64_t) + sizeof(uint16_t));
		LittleEndianCodec::writeU64(record.bytes, 0U, vectorId);
		rawRecords.push_back(std::move(record));
	}
	VectorPageStore vectors(io, *metadata);
	const uint16_t rawSlots = VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(rawRecords.front().bytes.size()));
	const size_t rawPageCount =
			(static_cast<size_t>(vectorCount) - 1U) / static_cast<size_t>(rawSlots) + 1U;
	ASSERT_LE(rawPageCount, static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT));
	const VectorRawMaterializationResult rawMaterialized = vectors.materializeRaw(
			INDEX_ID, rawRecords,
			{PhysicalSlotId(40U), PhysicalSlotId(40U), static_cast<size_t>(vectorCount), rawPageCount},
			LogSequenceNumber(3U));
	ASSERT_EQ(rawMaterialized.record_count, vectorCount);

	SlottedPagePool indexPages(io, *metadata);
	BTreePageStore indexes(indexPages, *metadata);
	std::vector<BTreePageEntry> nodeEntries;
	std::vector<BTreePageEntry> reverseEntries;
	nodeEntries.reserve(static_cast<size_t>(vectorCount));
	reverseEntries.reserve(static_cast<size_t>(vectorCount));
	for (uint64_t vectorId = 1U; vectorId <= vectorCount; ++vectorId) {
		nodeEntries.push_back({VectorSemanticCodec::nodeMappingKey(1000U + vectorId), vectorId});
		reverseEntries.push_back({VectorNodeMapping::key(vectorId), 1000U + vectorId});
	}
	const BulkBTreeBuildOptions nodeBuildOptions{
			PhysicalSlotId(41U), PhysicalSlotId(41U), INDEX_ID, static_cast<size_t>(vectorCount), 4U};
	const BulkBTreeBuildOptions reverseBuildOptions{
			PhysicalSlotId(42U), PhysicalSlotId(42U), REVERSE_INDEX_ID, static_cast<size_t>(vectorCount), 4U};
	const BulkBTreeBuildResult nodeBuild = BulkBTreeBuilder::build(
			indexes, *metadata, INDEX_ID, orderedSource(std::move(nodeEntries)), nodeBuildOptions, LogSequenceNumber(5U));
	const BulkBTreeBuildResult reverseBuild =
			BulkBTreeBuilder::build(indexes, *metadata, REVERSE_INDEX_ID, orderedSource(std::move(reverseEntries)),
									reverseBuildOptions, LogSequenceNumber(6U));
	ASSERT_EQ(nodeBuild.root_level, 1U);
	ASSERT_EQ(reverseBuild.root_level, 1U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation nodeRoot = metadata->findIndexRoot(snapshot, INDEX_ID);
	const IndexRootLocation reverseRoot = metadata->findIndexRoot(snapshot, REVERSE_INDEX_ID);
	snapshot.release();
	VectorManifest expectedManifest;
	expectedManifest.index_id = INDEX_ID;
	expectedManifest.page_id = PageId(701U);
	expectedManifest.generation = Generation(1U);
	expectedManifest.committed_lsn = LogSequenceNumber(7U);
	expectedManifest.dimensions = 1U;
	expectedManifest.raw_write_tail_page_id = rawMaterialized.pages.back().page_id;
	expectedManifest.node_mapping_root_page_id = nodeRoot.root_page_id;
	expectedManifest.node_mapping_root_generation = nodeRoot.root_generation;
	expectedManifest.vector_node_mapping_index_id = reverseRoot.index_id;
	expectedManifest.vector_node_mapping_root_page_id = reverseRoot.root_page_id;
	expectedManifest.vector_node_mapping_root_generation = reverseRoot.root_generation;
	for (const VectorRawMaterializationPage &page: rawMaterialized.pages) {
		expectedManifest.entries.push_back({page.page_id, VectorStorageKind::VSK_RAW, page.record_count});
	}
	const PageOwnerHeader manifestPageOwner{
			expectedManifest.page_id,		  expectedManifest.index_id,   PageOwnerKind::POK_VECTOR_MANIFEST, 0U,
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, expectedManifest.generation, expectedManifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = manifestSegmentId;
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = INDEX_ID;
	manifestRequest.generation = expectedManifest.generation;
	manifestRequest.page_id = expectedManifest.page_id;
	manifestRequest.page_owner_id = INDEX_ID;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestPageOwner, expectedManifest),
								  PhysicalSlotId(80U), PhysicalSlotId(80U), expectedManifest.committed_lsn)
						.has_value());

	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io, *metadata).scan();
	EXPECT_EQ(report.vector_locations, vectorCount);
	EXPECT_EQ(report.vector_manifests, 1U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	SharedPagePoolAllocator pageReader(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents rootContents = pageReader.read(current, nodeRoot.root_page_id);
	const BTreePage originalRoot = BTreePage::open(rootContents.owner, rootContents.payload);
	ASSERT_EQ(originalRoot.header().kind, BTreeNodeKind::BPN_INTERNAL);
	ASSERT_EQ(originalRoot.header().level, 1U);
	ASSERT_FALSE(originalRoot.entries().empty());
	const std::vector<BTreePageEntry> rootEntries = originalRoot.entries();
	{
		BTreePageHeader corruptHeader = originalRoot.header();
		corruptHeader.entry_count = 0U;
		BTreePage corrupt = BTreePage::create(rootContents.owner, corruptHeader);
		std::vector<BTreePageEntry> entries = rootEntries;
		entries.front().key = VectorSemanticCodec::nodeMappingKey(1001U);
		for (BTreePageEntry &entry: entries) {
			corrupt.insert(std::move(entry));
		}
		overwritePagePayload(io, *metadata, nodeRoot.root_page_id, corrupt.serialize());
		EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
		overwritePagePayload(io, *metadata, nodeRoot.root_page_id, originalRoot.serialize());
	}
	{
		BTreePageHeader corruptHeader = originalRoot.header();
		corruptHeader.leftmost_child = nodeRoot.root_page_id;
		corruptHeader.entry_count = 0U;
		BTreePage corrupt = BTreePage::create(rootContents.owner, corruptHeader);
		for (const BTreePageEntry &entry: rootEntries) {
			corrupt.insert(entry);
		}
		overwritePagePayload(io, *metadata, nodeRoot.root_page_id, corrupt.serialize());
		EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
		overwritePagePayload(io, *metadata, nodeRoot.root_page_id, originalRoot.serialize());
	}
	const PageId leftLeafId = originalRoot.header().leftmost_child;
	const SharedPagePoolContents leftLeafContents = pageReader.read(current, leftLeafId);
	const BTreePage originalLeftLeaf = BTreePage::open(leftLeafContents.owner, leftLeafContents.payload);
	ASSERT_EQ(originalLeftLeaf.header().kind, BTreeNodeKind::BPN_LEAF);
	ASSERT_TRUE(originalLeftLeaf.header().right_sibling.isValid());
	BTreePageHeader corruptLeafHeader = originalLeftLeaf.header();
	corruptLeafHeader.right_sibling = {};
	corruptLeafHeader.entry_count = 0U;
	BTreePage corruptLeftLeaf = BTreePage::create(leftLeafContents.owner, corruptLeafHeader);
	for (const BTreePageEntry &entry: originalLeftLeaf.entries()) {
		corruptLeftLeaf.insert(entry);
	}
	current.release();
	overwritePagePayload(io, *metadata, leftLeafId, corruptLeftLeaf.serialize());
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
	overwritePagePayload(io, *metadata, leftLeafId, originalLeftLeaf.serialize());
}

TEST_F(RelocatableIntegrityScannerTest, ScansIndexPqVectorAndCodebookLayouts) {
	constexpr uint64_t PQ_MAPPING_INDEX_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::vector<SegmentId> poolSegmentIds = metadata->allocateLogicalSegmentIds(4U, LogSequenceNumber(2U));
	SlottedPagePool slottedPages(io, *metadata);
	BTreePageStore indexes(slottedPages, *metadata);
	SlottedPagePoolAllocationRequest indexRequest;
	indexRequest.segment_id = poolSegmentIds[0];
	indexRequest.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	indexRequest.pool_owner_id = PQ_MAPPING_INDEX_ID;
	indexRequest.generation = Generation(1);
	indexRequest.page_id = PageId(401);
	indexRequest.page_owner_id = PQ_MAPPING_INDEX_ID;
	indexRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	const PageOwnerHeader indexOwner{indexRequest.page_id,
									 indexRequest.page_owner_id,
									 indexRequest.page_owner_kind,
									 0,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 indexRequest.generation,
									 LogSequenceNumber(3)};
	BTreePageHeader indexHeader;
	indexHeader.index_id = indexOwner.owner_id;
	indexHeader.page_id = indexOwner.page_id;
	indexHeader.generation = indexOwner.generation;
	BTreePage indexPage = BTreePage::create(indexOwner, indexHeader);
	indexPage.insert({{std::byte{'k'}}, 41});
	ASSERT_TRUE(indexes.allocate(indexRequest, indexPage, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(3))
						.has_value());
	const IndexRootLocation root{indexHeader.index_id, indexHeader.page_id, indexHeader.generation};
	static_cast<void>(indexes.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(4)));

	VectorPageHeader pqHeader;
	pqHeader.index_id = 19;
	pqHeader.page_id = PageId(402);
	pqHeader.generation = Generation(1);
	pqHeader.kind = VectorStorageKind::VSK_PQ;
	pqHeader.record_bytes = 8;
	pqHeader.slot_capacity = 2;
	pqHeader.bitmap_bytes = 1;
	pqHeader.committed_lsn = LogSequenceNumber(5);
	pqHeader.codebook_generation = Generation(1);
	const PageOwnerHeader pqOwner{pqHeader.page_id,
								  pqHeader.index_id,
								  PageOwnerKind::POK_VECTOR_PQ,
								  0,
								  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								  pqHeader.generation,
								  pqHeader.committed_lsn};
	VectorPage pqPage = VectorPage::create(pqOwner, pqHeader);
	std::vector<std::byte> pqRecord(pqHeader.record_bytes, std::byte{0x3A});
	LittleEndianCodec::writeU64(pqRecord, 0U, 700U);
	const uint16_t pqSlot = pqPage.insert(pqRecord);
	VectorPageStore vectors(io, *metadata);
	SharedPagePoolAllocationRequest pqRequest;
	pqRequest.segment_id = poolSegmentIds[1];
	pqRequest.pool_kind = SegmentKind::SGK_VECTOR_PQ_POOL;
	pqRequest.pool_owner_id = pqHeader.index_id;
	pqRequest.generation = pqHeader.generation;
	pqRequest.page_id = pqHeader.page_id;
	pqRequest.page_owner_id = pqHeader.index_id;
	pqRequest.page_owner_kind = pqOwner.kind;
	ASSERT_TRUE(vectors.allocate(pqRequest, pqPage, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(5))
						.has_value());
	const VectorLocation vectorLocation{700, pqHeader.page_id, pqSlot, pqPage.slotGeneration(pqSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&vectorLocation, 1), {}, LogSequenceNumber(6)));

	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest codebook;
	codebook.codebook_id = 20;
	codebook.manifest_segment_id = poolSegmentIds[2];
	codebook.generation = Generation(1);
	codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebook.dimensions = 2;
	codebook.subspace_count = 1;
	codebook.centroid_count = 3;
	static_cast<void>(codebooks.writeImmutable(codebook, std::vector<std::byte>(24U, std::byte{0x5A}),
											   PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(7)));

	VectorManifest expectedManifest;
	expectedManifest.index_id = pqHeader.index_id;
	expectedManifest.page_id = PageId(403);
	expectedManifest.generation = Generation(1);
	expectedManifest.committed_lsn = LogSequenceNumber(8);
	expectedManifest.dimensions = 2;
	expectedManifest.codebook_id = codebook.codebook_id;
	expectedManifest.codebook_generation = codebook.generation;
	expectedManifest.entry_point_vector_id = vectorLocation.vector_id;
	expectedManifest.pq_mapping_index_id = indexHeader.index_id;
	expectedManifest.pq_mapping_root_page_id = indexHeader.page_id;
	expectedManifest.pq_mapping_root_generation = indexHeader.generation;
	expectedManifest.entries = {{pqHeader.page_id, VectorStorageKind::VSK_PQ, 1}};
	const PageOwnerHeader manifestPageOwner{
			expectedManifest.page_id,		  expectedManifest.index_id,   PageOwnerKind::POK_VECTOR_MANIFEST, 0,
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, expectedManifest.generation, expectedManifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = poolSegmentIds[3];
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = expectedManifest.index_id;
	manifestRequest.generation = expectedManifest.generation;
	manifestRequest.page_id = expectedManifest.page_id;
	manifestRequest.page_owner_id = expectedManifest.index_id;
	manifestRequest.page_owner_kind = manifestPageOwner.kind;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestPageOwner, expectedManifest),
								  PhysicalSlotId(43), PhysicalSlotId(43), LogSequenceNumber(8))
						.has_value());

	RelocatableIntegrityScanner scanner(io, *metadata);
	const RelocatableIntegrityReport report = scanner.scan();
	EXPECT_EQ(report.generation, Generation(8));
	EXPECT_EQ(report.page_locations, 3U);
	EXPECT_EQ(report.extent_locations, 1U);
	EXPECT_EQ(report.vector_locations, 1U);
	EXPECT_EQ(report.vector_manifests, 1U);
	{
		VectorPage corrupt = VectorPage::create(pqOwner, pqHeader);
		std::vector<std::byte> mismatchedRecord(pqHeader.record_bytes, std::byte{0x3A});
		LittleEndianCodec::writeU64(mismatchedRecord, 0U, 701U);
		static_cast<void>(corrupt.insert(mismatchedRecord));
		overwritePagePayload(io, *metadata, pqHeader.page_id, corrupt.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		overwritePagePayload(io, *metadata, pqHeader.page_id, pqPage.serialize());
	}

	const auto verifyAllocatorBinding = [&](PageId pageId, LogSequenceNumber corruptLsn, LogSequenceNumber restoreLsn) {
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation current = metadata->findPageLocation(snapshot, pageId);
		PageLocation stale = current;
		++stale.free_bytes;
		static_cast<void>(metadata->upsertLocations(std::span(&stale, 1), {}, corruptLsn));
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		static_cast<void>(metadata->upsertLocations(std::span(&current, 1), {}, restoreLsn));
	};
	verifyAllocatorBinding(indexHeader.page_id, LogSequenceNumber(10), LogSequenceNumber(11));
	verifyAllocatorBinding(pqHeader.page_id, LogSequenceNumber(12), LogSequenceNumber(13));
	verifyAllocatorBinding(expectedManifest.page_id, LogSequenceNumber(14), LogSequenceNumber(15));
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x999);
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.pq_mapping_root_generation = Generation(2);
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.pq_mapping_root_page_id = PageId(404);
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.entries.front().record_count = 2;
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.codebook_id = 999;
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.codebook_id = 0;
		corrupt.codebook_generation = {};
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.codebook_generation = Generation(2);
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}
	{
		VectorManifest corrupt = expectedManifest;
		corrupt.entries.clear();
		corrupt.codebook_generation = Generation(2);
		const VectorManifestPage page = VectorManifestPage::create(manifestPageOwner, corrupt);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, page.serialize());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const VectorManifestPage restore = VectorManifestPage::create(manifestPageOwner, expectedManifest);
		overwritePagePayload(io, *metadata, expectedManifest.page_id, restore.serialize());
	}

	{
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PhysicalSegmentContents contents = physical.read(snapshot, codebook.manifest_segment_id);
		const VectorCodebookExtentHeader original = VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
		VectorCodebookExtentHeaderCodec::Bytes corrupt = VectorCodebookExtentHeaderCodec::encode(original);
		LittleEndianCodec::writeU64(corrupt, 16U, 999U);
		LittleEndianCodec::writeU32(corrupt, 48U, 0U);
		LittleEndianCodec::writeU32(corrupt, 48U, graph::utils::calculateCrc(corrupt.data(), corrupt.size()));
		overwriteExtentHeader(io, *metadata, codebook.manifest_segment_id, corrupt);
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		overwriteExtentHeader(io, *metadata, codebook.manifest_segment_id,
							  VectorCodebookExtentHeaderCodec::encode(original));
	}
}


TEST_F(RelocatableIntegrityScannerTest, RejectsManifestPagesMissingFromTheManifestDirectory) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expectedManifest = manifest();
	const PageOwnerHeader owner = manifestOwner(expectedManifest);
	SharedPagePoolAllocationRequest request = manifestRequest(expectedManifest);
	const VectorManifestPage page = VectorManifestPage::create(owner, expectedManifest);
	request.payload = page.serialize();
	SharedPagePoolAllocator pages(io, *metadata);
	ASSERT_TRUE(pages.allocateNewPoolPage(request, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(6))
						.has_value());
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsDuplicateManifestIndexOwnershipAcrossPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expectedManifest = manifest();
	const PageOwnerHeader firstOwner = manifestOwner(expectedManifest);
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest(expectedManifest),
								  VectorManifestPage::create(firstOwner, expectedManifest), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(6))
						.has_value());
	VectorManifest duplicate = expectedManifest;
	duplicate.page_id = PageId(103);
	duplicate.committed_lsn = LogSequenceNumber(7);
	const PageOwnerHeader duplicateOwner = manifestOwner(duplicate);
	SharedPagePoolAllocationRequest duplicateRequest = manifestRequest(duplicate);
	duplicateRequest.segment_id = SegmentId(74);
	const VectorManifestPage duplicatePage = VectorManifestPage::create(duplicateOwner, duplicate);
	duplicateRequest.payload = duplicatePage.serialize();
	SharedPagePoolAllocator pages(io, *metadata);
	ASSERT_TRUE(
			pages.allocateNewPoolPage(duplicateRequest, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(7))
					.has_value());
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsManifestReferencesToAbsentVectorPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expectedManifest = manifest();
	expectedManifest.entries = {{PageId(201), VectorStorageKind::VSK_RAW, 1}};
	const PageOwnerHeader owner = manifestOwner(expectedManifest);
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest(expectedManifest),
								  VectorManifestPage::create(owner, expectedManifest), PhysicalSlotId(40),
								  PhysicalSlotId(40), LogSequenceNumber(6))
						.has_value());
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsNoncanonicalPropertyPayload) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const PageOwnerHeader owner{
			PageId(301),		 99, PageOwnerKind::POK_PROPERTY, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1),
			LogSequenceNumber(2)};
	SlottedPage page = SlottedPage::create(owner);
	const TestSlottedPageRecord property{500U, {std::byte{0x11}, std::byte{0x22}}};
	const SlottedSlotReference slot = page.insert(TestSlottedPageRecordCodec::encode(property));
	SlottedPagePool pages(io, *metadata);
	SlottedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(77);
	request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.pool_owner_id = 99;
	request.generation = Generation(1);
	request.page_id = owner.page_id;
	request.page_owner_id = owner.owner_id;
	request.page_owner_kind = owner.kind;
	ASSERT_TRUE(pages.allocatePage(request, page, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	{
		const MetadataSnapshotPin ownerSnapshot = metadata->openSnapshot();
		SharedPagePoolAllocator pool(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		const PageOwnerHeader original = pool.read(ownerSnapshot, owner.page_id).owner;
		PageOwnerHeader retired = original;
		retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
		const auto bytes = PageOwnerHeaderCodec::encode(retired);
		io->writeAt(pagePayloadOffset(*metadata, owner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(),
					bytes.size());
		EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
		const auto restored = PageOwnerHeaderCodec::encode(original);
		io->writeAt(pagePayloadOffset(*metadata, owner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES, restored.data(),
					restored.size());
	}
	const MetadataSnapshotPin validSnapshot = metadata->openSnapshot();
	const PageLocation validPageLocation = metadata->findPageLocation(validSnapshot, owner.page_id);
	PageLocation staleFreeSpace = validPageLocation;
	++staleFreeSpace.free_bytes;
	static_cast<void>(metadata->upsertLocations(std::span(&staleFreeSpace, 1), {}, LogSequenceNumber(4)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(metadata->upsertLocations(std::span(&validPageLocation, 1), {}, LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage corrupt = pages.read(snapshot, owner.page_id);
	TestSlottedPageRecord mismatched = property;
	mismatched.logical_id = 501U;
	corrupt.overwrite(slot, TestSlottedPageRecordCodec::encode(mismatched));
	const std::vector<std::byte> bytes = corrupt.serialize();
	overwritePagePayload(io, *metadata, owner.page_id, bytes);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsDuplicateStateRecordsAcrossStatePages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	const StateRecord record{91U, "system.duplicate", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x5A}}};
	static_cast<void>(states.upsert(record, {PhysicalSlotId(40U), PhysicalSlotId(40U), 801U}));
	const LogSequenceNumber duplicateLsn = nextCheckpointLsn(*metadata);

	const PageOwnerHeader duplicateOwner{
			PageId(802U),	802U,		 PageOwnerKind::POK_STATE, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			Generation(1U), duplicateLsn};
	SlottedPage duplicatePage = SlottedPage::create(duplicateOwner);
	static_cast<void>(duplicatePage.insert(StateRecordCodec::encode(record)));
	SlottedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(82U);
	request.pool_kind = SegmentKind::SGK_STATE_PAGE_POOL;
	request.pool_owner_id = duplicateOwner.owner_id;
	request.generation = duplicateOwner.generation;
	request.page_id = duplicateOwner.page_id;
	request.page_owner_id = duplicateOwner.owner_id;
	request.page_owner_kind = duplicateOwner.kind;
	ASSERT_TRUE(pages.allocatePage(request, duplicatePage, PhysicalSlotId(41U), PhysicalSlotId(41U), duplicateLsn)
						.has_value());

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, RejectsDerivedAdjacencyMappingAndRecordCorruption) {
	constexpr uint64_t INDEX_ID = 51U;
	constexpr uint64_t MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());

	VectorPageHeader rawHeader;
	rawHeader.index_id = INDEX_ID;
	rawHeader.page_id = PageId(831U);
	rawHeader.generation = Generation(1U);
	rawHeader.kind = VectorStorageKind::VSK_RAW;
	rawHeader.record_bytes = sizeof(uint64_t) + sizeof(uint16_t);
	rawHeader.slot_capacity = 2U;
	rawHeader.bitmap_bytes = 1U;
	rawHeader.committed_lsn = LogSequenceNumber(2U);
	const PageOwnerHeader rawOwner{rawHeader.page_id,
								   rawHeader.index_id,
								   PageOwnerKind::POK_VECTOR_RAW,
								   0U,
								   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								   rawHeader.generation,
								   rawHeader.committed_lsn};
	VectorPage rawPage = VectorPage::create(rawOwner, rawHeader);
	std::array<std::byte, sizeof(uint64_t) + sizeof(uint16_t)> rawRecord{};
	LittleEndianCodec::writeU64(rawRecord, 0U, 701U);
	const uint16_t rawSlot = rawPage.insert(rawRecord);
	VectorPageStore vectors(io, *metadata);
	SharedPagePoolAllocationRequest rawRequest;
	rawRequest.segment_id = SegmentId(86U);
	rawRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawRequest.pool_owner_id = INDEX_ID;
	rawRequest.generation = rawHeader.generation;
	rawRequest.page_id = rawHeader.page_id;
	rawRequest.page_owner_id = INDEX_ID;
	rawRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(vectors.allocate(rawRequest, rawPage, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());
	const VectorLocation rawLocation{701U, rawHeader.page_id, rawSlot, rawPage.slotGeneration(rawSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&rawLocation, 1U), {}, LogSequenceNumber(3U)));

	const PageOwnerHeader adjacencyOwner{PageId(832U),
										 VectorAdjacencyPageStore::derivedOwnerId(INDEX_ID),
										 PageOwnerKind::POK_VECTOR_ADJACENCY,
										 0U,
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										 Generation(1U),
										 LogSequenceNumber(4U)};
	const auto derivedPage = [&](const VectorAdjacencyRecord &record) {
		SlottedPage page = SlottedPage::create(adjacencyOwner);
		static_cast<void>(page.insert(VectorAdjacencyCodec::encode(record)));
		return page;
	};
	const SlottedPage initialAdjacency = derivedPage({701U, {}});
	EXPECT_NO_THROW(static_cast<void>(SlottedPage::open(initialAdjacency.serialize())));
	SharedPagePoolAllocationRequest adjacencyRequest;
	adjacencyRequest.segment_id = SegmentId(87U);
	adjacencyRequest.pool_kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
	adjacencyRequest.pool_owner_id = INDEX_ID;
	adjacencyRequest.generation = adjacencyOwner.generation;
	adjacencyRequest.page_id = adjacencyOwner.page_id;
	adjacencyRequest.page_owner_id = adjacencyOwner.owner_id;
	adjacencyRequest.page_owner_kind = adjacencyOwner.kind;
	adjacencyRequest.page_free_bytes = initialAdjacency.stats().contiguous_free_bytes;
	const std::vector<std::byte> initialAdjacencyBytes = initialAdjacency.serialize();
	adjacencyRequest.payload = initialAdjacencyBytes;
	SharedPagePoolAllocator sharedPages(io, *metadata);
	ASSERT_TRUE(sharedPages
						.allocateNewPoolPage(adjacencyRequest, PhysicalSlotId(41U), PhysicalSlotId(41U),
											 adjacencyOwner.committed_lsn)
						.has_value());
	{
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const SharedPagePoolAllocator reader(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		const SharedPagePoolContents contents = reader.read(snapshot, adjacencyOwner.page_id);
		EXPECT_NO_THROW(static_cast<void>(SlottedPage::open(contents.owner, contents.payload)));
	}

	SlottedPagePool indexPages(io, *metadata);
	BTreePageStore trees(indexPages, *metadata);
	SlottedPagePoolAllocationRequest mappingRequest;
	mappingRequest.segment_id = SegmentId(88U);
	mappingRequest.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	mappingRequest.pool_owner_id = MAPPING_INDEX_ID;
	mappingRequest.generation = Generation(1U);
	mappingRequest.page_id = PageId(833U);
	mappingRequest.page_owner_id = MAPPING_INDEX_ID;
	mappingRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	const PageOwnerHeader mappingOwner{mappingRequest.page_id,			 MAPPING_INDEX_ID, PageOwnerKind::POK_INDEX, 0U,
									   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U),   LogSequenceNumber(5U)};
	BTreePageHeader mappingHeader;
	mappingHeader.index_id = MAPPING_INDEX_ID;
	mappingHeader.page_id = mappingRequest.page_id;
	mappingHeader.generation = Generation(1U);
	BTreePage mapping = BTreePage::create(mappingOwner, mappingHeader);
	mapping.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	ASSERT_TRUE(trees.allocate(mappingRequest, mapping, PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(5U))
						.has_value());
	const IndexRootLocation mappingRoot{MAPPING_INDEX_ID, mappingHeader.page_id, mappingHeader.generation};
	static_cast<void>(trees.publishRoots(std::span(&mappingRoot, 1U), {}, LogSequenceNumber(6U)));

	VectorManifest manifest;
	manifest.index_id = INDEX_ID;
	manifest.page_id = PageId(834U);
	manifest.generation = Generation(1U);
	manifest.committed_lsn = LogSequenceNumber(7U);
	manifest.dimensions = 1U;
	manifest.raw_write_tail_page_id = rawHeader.page_id;
	manifest.entry_point_vector_id = 701U;
	manifest.adjacency_generation = Generation(1U);
	manifest.adjacency_mapping_index_id = mappingRoot.index_id;
	manifest.adjacency_mapping_root_page_id = mappingRoot.root_page_id;
	manifest.adjacency_mapping_root_generation = mappingRoot.root_generation;
	manifest.entries = {{rawHeader.page_id, VectorStorageKind::VSK_RAW, 1U}};
	const PageOwnerHeader manifestOwner{manifest.page_id,
										manifest.index_id,
										PageOwnerKind::POK_VECTOR_MANIFEST,
										0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										manifest.generation,
										manifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = SegmentId(89U);
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = INDEX_ID;
	manifestRequest.generation = manifest.generation;
	manifestRequest.page_id = manifest.page_id;
	manifestRequest.page_owner_id = INDEX_ID;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestOwner, manifest),
								  PhysicalSlotId(43U), PhysicalSlotId(43U), manifest.committed_lsn)
						.has_value());

	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	const auto replaceManifest = [&](const VectorManifest &replacement) {
		overwritePagePayload(io, *metadata, manifest.page_id,
							 VectorManifestPage::create(manifestOwner, replacement).serialize());
	};
	const auto replaceMapping = [&](const BTreePage &replacement) {
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		PageLocation location = metadata->findPageLocation(snapshot, mappingHeader.page_id);
		location.free_bytes = replacement.slottedPage().stats().contiguous_free_bytes;
		overwritePagePayload(io, *metadata, mappingHeader.page_id, replacement.serialize());
		static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	};
	const auto replaceAdjacency = [&](const SlottedPage &replacement) {
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		PageLocation location = metadata->findPageLocation(snapshot, adjacencyOwner.page_id);
		location.free_bytes = replacement.stats().contiguous_free_bytes;
		overwritePagePayload(io, *metadata, adjacencyOwner.page_id, replacement.serialize());
		static_cast<void>(metadata->upsertLocations(std::span(&location, 1U), {}, nextCheckpointLsn(*metadata)));
	};

	VectorManifest wrongRoot = manifest;
	wrongRoot.adjacency_mapping_root_generation = Generation(2U);
	replaceManifest(wrongRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(manifest);
	BTreePageHeader leftSiblingHeader = mappingHeader;
	leftSiblingHeader.left_sibling = PageId(900U);
	BTreePage leftSiblingRoot = BTreePage::create(mappingOwner, leftSiblingHeader);
	leftSiblingRoot.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	replaceMapping(leftSiblingRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	BTreePageHeader rightSiblingHeader = mappingHeader;
	rightSiblingHeader.right_sibling = PageId(901U);
	BTreePage rightSiblingRoot = BTreePage::create(mappingOwner, rightSiblingHeader);
	rightSiblingRoot.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	replaceMapping(rightSiblingRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceMapping(mapping);
	BTreePageHeader missingChildHeader = mappingHeader;
	missingChildHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	missingChildHeader.level = 1U;
	missingChildHeader.leftmost_child = PageId(902U);
	BTreePage missingChildRoot = BTreePage::create(mappingOwner, missingChildHeader);
	missingChildRoot.insert({VectorAdjacencyMapping::key(701U), PageId(903U).value()});
	replaceMapping(missingChildRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	const LogSequenceNumber childLsn = nextCheckpointLsn(*metadata);
	SlottedPagePoolAllocationRequest childRequest = mappingRequest;
	childRequest.segment_id = SegmentId(90U);
	childRequest.page_id = PageId(904U);
	const PageOwnerHeader childOwner{
			childRequest.page_id, MAPPING_INDEX_ID, PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			Generation(1U),		  childLsn};
	BTreePageHeader childHeader;
	childHeader.index_id = MAPPING_INDEX_ID;
	childHeader.page_id = childRequest.page_id;
	childHeader.generation = Generation(1U);
	BTreePage child = BTreePage::create(childOwner, childHeader);
	child.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	ASSERT_TRUE(trees.allocate(childRequest, child, PhysicalSlotId(44U), PhysicalSlotId(44U), childLsn).has_value());
	BTreePageHeader mismatchedLevelHeader = mappingHeader;
	mismatchedLevelHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	mismatchedLevelHeader.level = 2U;
	mismatchedLevelHeader.leftmost_child = childHeader.page_id;
	BTreePage mismatchedLevelRoot = BTreePage::create(mappingOwner, mismatchedLevelHeader);
	mismatchedLevelRoot.insert({VectorAdjacencyMapping::key(701U), PageId(905U).value()});
	replaceMapping(mismatchedLevelRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceMapping(mapping);
	BTreePage incompleteMapping = BTreePage::create(mappingOwner, mappingHeader);
	replaceMapping(incompleteMapping);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceMapping(mapping);
	BTreePage wrongKey = BTreePage::create(mappingOwner, mappingHeader);
	wrongKey.insert({VectorAdjacencyMapping::key(700U), adjacencyOwner.page_id.value()});
	replaceMapping(wrongKey);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceMapping(mapping);
	BTreePage zeroPageValue = BTreePage::create(mappingOwner, mappingHeader);
	zeroPageValue.insert({VectorAdjacencyMapping::key(701U), 0U});
	replaceMapping(zeroPageValue);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceMapping(mapping);
	SlottedPage unorderedAdjacency = SlottedPage::create(adjacencyOwner);
	static_cast<void>(unorderedAdjacency.insert(VectorAdjacencyCodec::encode({702U, {}})));
	static_cast<void>(unorderedAdjacency.insert(VectorAdjacencyCodec::encode({701U, {}})));
	replaceAdjacency(unorderedAdjacency);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceAdjacency(initialAdjacency);
	const SlottedPage invalidNeighbor = derivedPage({701U, {702U}});
	replaceAdjacency(invalidNeighbor);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceAdjacency(initialAdjacency);
	const SlottedPage missingVector = derivedPage({702U, {}});
	replaceAdjacency(missingVector);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTest, WorkspaceQuotaDenialPreservesFileAndReleasesEveryAdmittedNode) {
    using namespace graph::query::execution::detail;
    auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
    static_cast<void>(writeMultiLevelLargeValue(io, *metadata));
    const auto expected = RelocatableIntegrityScanner(io, *metadata).scan();
    const auto generation = metadata->openSnapshot().superblock().generation;
    const auto before = io->stats();
    const auto fileBytes = std::filesystem::file_size(path);
    constexpr size_t maximum = 16U * 1024U * 1024U;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_MAINTENANCE, maximum);
    {
        auto tiny = std::make_shared<PrepaidMemoryResource>(account, 256U, std::pmr::new_delete_resource());
        EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, tiny).scan()), ResourceAdmissionDenied);
        EXPECT_GT(tiny->denials(), 0U);
        EXPECT_EQ(tiny->used(), 0U);
        EXPECT_EQ(account->used(), 256U);
    }
    EXPECT_EQ(account->used(), 0U);
    {
        auto workspace = std::make_shared<PrepaidMemoryResource>(account, maximum, std::pmr::new_delete_resource());
        EXPECT_EQ(RelocatableIntegrityScanner(io, *metadata, workspace).scan(), expected);
        EXPECT_GT(workspace->peakAdmitted(), 256U);
        EXPECT_EQ(workspace->used(), 0U);
        EXPECT_EQ(account->used(), maximum);
    }
    EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock().generation, generation);
    EXPECT_EQ(io->stats().write_calls, before.write_calls);
    EXPECT_EQ(io->stats().reserve_calls, before.reserve_calls);
    EXPECT_EQ(io->stats().sync_calls, before.sync_calls);
    EXPECT_EQ(std::filesystem::file_size(path), fileBytes);
}

TEST_F(RelocatableIntegrityScannerTest, RelationshipChecksDoNotRetainEveryPagePayload) {
    using namespace graph::query::execution::detail;
    auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
    SlottedPagePool pages(io, *metadata);
    StateRecordStore states(pages, *metadata);
    const StateRecordStoreOptions options{PhysicalSlotId(40), PhysicalSlotId(300), 800};
    for (uint64_t id = 1U; id <= 128U; ++id) {
        const StateRecord record{id, "bounded.page", StateRecordStorageKind::SRS_INLINE,
            LargeValueId{}, std::vector<std::byte>(3000U, std::byte{0x41})};
        static_cast<void>(states.upsert(record, options));
    }
    const auto expected = RelocatableIntegrityScanner(io, *metadata).scan();
    ASSERT_EQ(expected.state_locations, 128U);
    ASSERT_GE(expected.page_locations, 128U);
    constexpr size_t maximum = 256U * 1024U;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_MAINTENANCE, maximum);
    auto workspace = std::make_shared<PrepaidMemoryResource>(account, maximum, std::pmr::new_delete_resource());
    const auto before = io->stats();
    const auto generation = metadata->openSnapshot().superblock().generation;
    EXPECT_EQ(RelocatableIntegrityScanner(io, *metadata, workspace).scan(), expected);
    EXPECT_EQ(workspace->used(), 0U);
    EXPECT_EQ(io->stats().write_calls, before.write_calls);
    EXPECT_EQ(metadata->openSnapshot().superblock().generation, generation);
}

TEST_F(RelocatableIntegrityScannerTest, CancellationBeforeAndDuringValidationReleasesThePinnedWorkspace) {
    using namespace graph::query::execution::detail;
    auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
    static_cast<void>(writeMultiLevelLargeValue(io, *metadata));
    constexpr size_t maximum = 1024U * 1024U;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_MAINTENANCE, maximum);
    auto workspace = std::make_shared<PrepaidMemoryResource>(account, maximum, std::pmr::new_delete_resource());
    struct Cancel final : CancellationCheckpoint {
        explicit Cancel(size_t limit) : limit(limit) {}
        void check() const override { if (++calls == limit) throw std::runtime_error("cancel integrity"); }
        size_t limit;
        mutable size_t calls = 0U;
    };
    const auto root = metadata->openSnapshot().superblock();
    for (const auto limit : {1U, 5U}) {
        Cancel cancel(limit);
        io->resetStats();
        EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, workspace).scan(&cancel)),
            std::runtime_error);
        EXPECT_EQ(cancel.calls, limit);
        EXPECT_EQ(workspace->used(), 0U);
        EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
        EXPECT_EQ(io->stats().write_calls, 0U);
        EXPECT_EQ(io->stats().reserve_calls, 0U);
        if (limit == 1U) EXPECT_EQ(io->stats().read_calls, 0U);
        EXPECT_EQ(metadata->openSnapshot().superblock(), root);
    }
}
