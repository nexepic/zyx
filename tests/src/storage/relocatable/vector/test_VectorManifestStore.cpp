/**
 * @file test_VectorManifestStore.cpp
 * @brief Atomic publication and recovery tests for relocatable vector manifests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

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

	VectorManifest manifest(uint64_t indexId = 17, PageId pageId = PageId(101), uint64_t lsn = 2) {
		VectorManifest result;
		result.index_id = indexId;
		result.page_id = pageId;
		result.generation = Generation(1);
		result.committed_lsn = LogSequenceNumber(lsn);
		result.dimensions = 16;
		result.entries = {{PageId(201), VectorStorageKind::VSK_RAW, 3}, {PageId(202), VectorStorageKind::VSK_PQ, 3}};
		return result;
	}

	PageOwnerHeader owner(const VectorManifest &manifest) {
		return {manifest.page_id,
				manifest.index_id,
				PageOwnerKind::POK_VECTOR_MANIFEST,
				0,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				manifest.generation,
				manifest.committed_lsn};
	}

	SharedPagePoolAllocationRequest request(const VectorManifest &manifest, SegmentId segmentId = SegmentId(71)) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		result.pool_owner_id = manifest.index_id;
		result.generation = manifest.generation;
		result.page_id = manifest.page_id;
		result.page_owner_id = manifest.index_id;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
		return result;
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	uint64_t manifestFrameOffset(RelocatableMetadataStore &metadata, PageId pageId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
														   pool.physical_slot) +
			   static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
	}

	void overwriteManifestFrame(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
								const PageOwnerHeader &frameOwner, const VectorManifestPage &page) {
		const uint64_t offset = manifestFrameOffset(metadata, pageId);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(frameOwner);
		io->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, page.serialize().data(), page.serialize().size());
	}

	SharedPagePoolAllocationRequest rawVectorRequest(SegmentId segmentId, PageId pageId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
		result.pool_owner_id = 17;
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = 17;
		result.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
		return result;
	}

	VectorPage rawVectorPage(const SharedPagePoolAllocationRequest &request, LogSequenceNumber lsn) {
		VectorPageHeader header;
		header.index_id = request.page_owner_id;
		header.page_id = request.page_id;
		header.generation = request.generation;
		header.kind = VectorStorageKind::VSK_RAW;
		header.record_bytes = 16;
		header.slot_capacity = 2;
		header.bitmap_bytes = 1;
		header.committed_lsn = lsn;
		const PageOwnerHeader pageOwner{header.page_id,
										header.index_id,
										PageOwnerKind::POK_VECTOR_RAW,
										0,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										header.generation,
										header.committed_lsn};
		return VectorPage::create(pageOwner, header);
	}

	[[nodiscard]] OrderedBTreeEntrySource sourceFor(std::vector<BTreePageEntry> entries) {
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

	[[nodiscard]] IndexRootLocation buildNodeMappingRoot(const std::shared_ptr<StorageIO> &io,
														 RelocatableMetadataStore &metadata, uint64_t indexId,
														 LogSequenceNumber checkpointLsn) {
		SlottedPagePool slotted(io, metadata);
		BTreePageStore trees(slotted, metadata);
		const std::vector<BTreePageEntry> entries = {{{std::byte{0x01U}}, 1U}};
		const BulkBTreeBuildOptions options{PhysicalSlotId(40U), PhysicalSlotId(95U), indexId, entries.size(), 1U};
		static_cast<void>(
				BulkBTreeBuilder::build(trees, metadata, indexId, sourceFor(entries), options, checkpointLsn));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
		snapshot.release();
		return root;
	}

	[[nodiscard]] IndexRootLocation buildVectorNodeMappingRoot(const std::shared_ptr<StorageIO> &io,
															   RelocatableMetadataStore &metadata,
															   uint64_t mappingIndexId,
															   LogSequenceNumber checkpointLsn) {
		SlottedPagePool slotted(io, metadata);
		BTreePageStore trees(slotted, metadata);
		const std::vector<BTreePageEntry> entries = {{VectorNodeMapping::key(1U), 1U}};
		VectorNodeMappingBuildOptions options;
		options.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, entries.size(), 1U};
		options.maximum_entries = entries.size();
		static_cast<void>(
				VectorNodeMapping::build(trees, metadata, mappingIndexId, sourceFor(entries), options, checkpointLsn));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, mappingIndexId);
		snapshot.release();
		return root;
	}

	[[nodiscard]] IndexRootLocation buildAdjacencyMappingRoot(const std::shared_ptr<StorageIO> &io,
															  RelocatableMetadataStore &metadata,
															  uint64_t mappingIndexId,
															  LogSequenceNumber checkpointLsn) {
		SlottedPagePool slotted(io, metadata);
		BTreePageStore trees(slotted, metadata);
		const std::array<VectorAdjacencyPageMapping, 1> mappings = {{{1U, PageId(101U)}}};
		VectorAdjacencyMappingBuildOptions options;
		options.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, mappings.size(), 1U};
		options.maximum_entries = mappings.size();
		static_cast<void>(
				VectorAdjacencyMapping::build(trees, metadata, mappingIndexId, mappings, options, checkpointLsn));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, mappingIndexId);
		snapshot.release();
		return root;
	}

	[[nodiscard]] IndexRootLocation buildPqMappingRoot(const std::shared_ptr<StorageIO> &io,
													   RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
													   LogSequenceNumber checkpointLsn) {
		SlottedPagePool slotted(io, metadata);
		BTreePageStore trees(slotted, metadata);
		const std::array<VectorPqPageMapping, 1> mappings = {{{1U, PageId(101U)}}};
		VectorPqMappingBuildOptions options;
		options.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, mappings.size(), 1U};
		options.maximum_entries = mappings.size();
		static_cast<void>(VectorPqMapping::build(trees, metadata, mappingIndexId, mappings, options, checkpointLsn));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, mappingIndexId);
		snapshot.release();
		return root;
	}

} // namespace

class RelocatableVectorManifestStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_vector_manifest_store_" + to_string(uuid));
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

TEST_F(RelocatableVectorManifestStoreTest, RecordDurableBeforeWalHeaderIsInvisibleAndCanBeRetried) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest expected = manifest();
	const VectorManifestPage page = VectorManifestPage::create(owner(expected), expected);
	VectorManifestStore store(io, *metadata);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.allocate(request(expected), page, PhysicalSlotId(40), PhysicalSlotId(40),
												  LogSequenceNumber(2), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin beforeRetry = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findVectorManifestOptional(beforeRetry, expected.index_id).has_value());
	beforeRetry.release();
	VectorManifestStore retryStore(io, *reopened);
	ASSERT_TRUE(
			retryStore.allocate(request(expected), page, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	MetadataSnapshotPin current = reopened->openSnapshot();
	EXPECT_EQ(retryStore.read(current, expected.index_id).manifest(), expected);
}

TEST_F(RelocatableVectorManifestStoreTest, AssignsManifestPageAndPoolIdentityInItsPublicationGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	VectorManifestStore store(io, *metadata);
	const VectorManifestMaterializationResult written =
			store.materialize(expected, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	EXPECT_TRUE(written.manifest.page_id.isValid());
	EXPECT_EQ(written.manifest.generation, Generation(1));
	EXPECT_EQ(written.manifest.committed_lsn, LogSequenceNumber(2));
	EXPECT_EQ(written.allocation.location.page_id, written.manifest.page_id);
	EXPECT_EQ(written.allocation.pool.owner_id, written.manifest.index_id);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, written.manifest.index_id).manifest(), written.manifest);

	expected = manifest();
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(expected, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMaterializationWithAnUnavailablePqMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.pq_mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expected.pq_mapping_root_page_id = PageId(51);
	expected.pq_mapping_root_generation = Generation(1);
	VectorManifestStore store(io, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(expected, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMismatchedPqMappingRootAndAcceptsItsPublishedIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	const IndexRootLocation mappingRoot = buildPqMappingRoot(io, *metadata, mappingIndexId, LogSequenceNumber(2U));
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.pq_mapping_index_id = mappingRoot.index_id;
	expected.pq_mapping_root_page_id = mappingRoot.root_page_id;
	expected.pq_mapping_root_generation = mappingRoot.root_generation;
	VectorManifestStore store(io, *metadata);

	VectorManifest wrongPage = expected;
	wrongPage.pq_mapping_root_page_id = PageId(mappingRoot.root_page_id.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(wrongPage, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	VectorManifest wrongGeneration = expected;
	wrongGeneration.pq_mapping_root_generation = Generation(mappingRoot.root_generation.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(store.materialize(wrongGeneration, PhysicalSlotId(40U), PhysicalSlotId(95U),
													LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);

	const VectorManifestMaterializationResult materialized =
			store.materialize(expected, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(3U));
	EXPECT_EQ(materialized.manifest.pq_mapping_index_id, mappingRoot.index_id);
}

TEST_F(RelocatableVectorManifestStoreTest, ValidatesManifestPoolCapacityAndPrivatePreparationContracts) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest initial = manifest();
	initial.page_id = {};
	initial.generation = {};
	initial.committed_lsn = {};
	initial.entries.clear();
	VectorManifestStore store(io, *metadata);
	const VectorManifestMaterializationResult materialized =
			store.materialize(initial, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(2U));
	const CatalogEntry pool = materialized.allocation.pool;
	EXPECT_FALSE(store.hasReplacementFrameCapacity({}, 1U));
	EXPECT_FALSE(store.hasReplacementFrameCapacity(pool, 0U));
	EXPECT_TRUE(store.hasReplacementFrameCapacity(pool, 1U));
	EXPECT_FALSE(store.hasReplacementFrameCapacity(
			pool, static_cast<size_t>(SharedPagePoolAllocator(io, *metadata).availableFrameCount(pool)) + 1U));

	const VectorManifestPage page = VectorManifestPage::create(owner(materialized.manifest), materialized.manifest);
	PageLocation replacement;
	expectFormatError(
			[&] {
				store.prepareReplacement(pool, materialized.allocation.location, {}, page, LogSequenceNumber(2U),
										 replacement);
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				PageLocation invalidInitial = materialized.allocation.location;
				store.prepareNewPoolReplacement(pool, page, LogSequenceNumber(2U), invalidInitial);
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				store.prepareReplacementSourceRetirement(pool, materialized.allocation.location, {},
														 LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMaterializationWithAnUnavailableVectorNodeMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.node_mapping_root_page_id = PageId(50U);
	expected.node_mapping_root_generation = Generation(1U);
	expected.vector_node_mapping_index_id = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expected.vector_node_mapping_root_page_id = PageId(51U);
	expected.vector_node_mapping_root_generation = Generation(1U);
	VectorManifestStore store(io, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(expected, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMismatchedVectorNodeMappingRootAndAcceptsItsPublishedIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const IndexRootLocation nodeRoot = buildNodeMappingRoot(io, *metadata, 17U, LogSequenceNumber(2U));
	const uint64_t mappingIndexId = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	const IndexRootLocation mappingRoot =
			buildVectorNodeMappingRoot(io, *metadata, mappingIndexId, LogSequenceNumber(3U));
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.node_mapping_root_page_id = nodeRoot.root_page_id;
	expected.node_mapping_root_generation = nodeRoot.root_generation;
	expected.vector_node_mapping_index_id = mappingRoot.index_id;
	expected.vector_node_mapping_root_page_id = mappingRoot.root_page_id;
	expected.vector_node_mapping_root_generation = mappingRoot.root_generation;
	VectorManifestStore store(io, *metadata);

	VectorManifest wrongPage = expected;
	wrongPage.vector_node_mapping_root_page_id = PageId(mappingRoot.root_page_id.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(wrongPage, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);
	VectorManifest wrongGeneration = expected;
	wrongGeneration.vector_node_mapping_root_generation = Generation(mappingRoot.root_generation.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(store.materialize(wrongGeneration, PhysicalSlotId(40U), PhysicalSlotId(95U),
													LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);

	const VectorManifestMaterializationResult materialized =
			store.materialize(expected, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(4U));
	EXPECT_EQ(materialized.manifest.vector_node_mapping_index_id, mappingRoot.index_id);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMaterializationWithAnUnavailableAdjacencyMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.entry_point_vector_id = 1U;
	expected.adjacency_generation = Generation(1U);
	expected.adjacency_mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	expected.adjacency_mapping_root_page_id = PageId(51U);
	expected.adjacency_mapping_root_generation = Generation(1U);
	VectorManifestStore store(io, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(expected, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsMismatchedAdjacencyMappingRootAndAcceptsItsPublishedIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const uint64_t mappingIndexId = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	const IndexRootLocation mappingRoot =
			buildAdjacencyMappingRoot(io, *metadata, mappingIndexId, LogSequenceNumber(2U));
	VectorManifest expected = manifest();
	expected.page_id = {};
	expected.generation = {};
	expected.committed_lsn = {};
	expected.entries.clear();
	expected.entry_point_vector_id = 1U;
	expected.adjacency_generation = Generation(1U);
	expected.adjacency_mapping_index_id = mappingRoot.index_id;
	expected.adjacency_mapping_root_page_id = mappingRoot.root_page_id;
	expected.adjacency_mapping_root_generation = mappingRoot.root_generation;
	VectorManifestStore store(io, *metadata);

	VectorManifest wrongPage = expected;
	wrongPage.adjacency_mapping_root_page_id = PageId(mappingRoot.root_page_id.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.materialize(wrongPage, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	VectorManifest wrongGeneration = expected;
	wrongGeneration.adjacency_mapping_root_generation = Generation(mappingRoot.root_generation.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(store.materialize(wrongGeneration, PhysicalSlotId(40U), PhysicalSlotId(95U),
													LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);

	const VectorManifestMaterializationResult materialized =
			store.materialize(expected, PhysicalSlotId(40U), PhysicalSlotId(95U), LogSequenceNumber(3U));
	EXPECT_EQ(materialized.manifest.adjacency_mapping_index_id, mappingRoot.index_id);
}

TEST_F(RelocatableVectorManifestStoreTest, WalHeaderDurablePublicationReplaysCompleteManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest expected = manifest();
	const VectorManifestPage page = VectorManifestPage::create(owner(expected), expected);
	VectorManifestStore store(io, *metadata);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.allocate(request(expected), page, PhysicalSlotId(40), PhysicalSlotId(40),
												  LogSequenceNumber(2), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	VectorManifestStore reopenedStore(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const VectorManifestLocation expectedLocation{expected.index_id, expected.page_id, expected.generation};
	EXPECT_EQ(reopened->findVectorManifest(snapshot, expected.index_id), expectedLocation);
	EXPECT_EQ(reopenedStore.read(snapshot, expected.index_id).manifest(), expected);
}

TEST_F(RelocatableVectorManifestStoreTest, MaterializesOnlyManifestsReferencingPublishedCodebookGenerations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 18;
	codebookRequest.manifest_segment_id = SegmentId(72);
	codebookRequest.generation = Generation(1);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2;
	codebookRequest.subspace_count = 1;
	codebookRequest.centroid_count = 3;
	const std::vector<std::byte> codebookBytes(24U, std::byte{0x5A});
	static_cast<void>(codebooks.writeImmutable(codebookRequest, codebookBytes, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(2)));
	VectorManifest expected = manifest(17, PageId(101), 3);
	expected.dimensions = 2;
	expected.codebook_id = codebookRequest.codebook_id;
	expected.codebook_generation = codebookRequest.generation;
	expected.entry_point_vector_id = 17;
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(expected), VectorManifestPage::create(owner(expected), expected),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, expected.index_id).manifest(), expected);
	snapshot.release();
	VectorManifest invalid = expected;
	invalid.index_id = 19;
	invalid.page_id = PageId(102);
	invalid.committed_lsn = LogSequenceNumber(4);
	invalid.codebook_generation = Generation(2);
	expectFormatError(
			[&] {
				static_cast<void>(store.allocate(request(invalid, SegmentId(73)),
												 VectorManifestPage::create(owner(invalid), invalid),
												 PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	invalid.codebook_generation = codebookRequest.generation;
	invalid.codebook_id = 99;
	expectFormatError(
			[&] {
				static_cast<void>(store.allocate(request(invalid, SegmentId(73)),
												 VectorManifestPage::create(owner(invalid), invalid),
												 PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, KeepsManifestCodebookReferenceValidAcrossPhysicalCodebookRelocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 18;
	codebookRequest.manifest_segment_id = SegmentId(72);
	codebookRequest.generation = Generation(1);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2;
	codebookRequest.subspace_count = 1;
	codebookRequest.centroid_count = 3;
	const std::vector<std::byte> codebookBytes(24U, std::byte{0x5A});
	static_cast<void>(codebooks.writeImmutable(codebookRequest, codebookBytes, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(2)));
	VectorManifest expected = manifest(17, PageId(101), 3);
	expected.dimensions = 2;
	expected.codebook_id = codebookRequest.codebook_id;
	expected.codebook_generation = codebookRequest.generation;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(request(expected, SegmentId(71)),
								  VectorManifestPage::create(owner(expected), expected), PhysicalSlotId(41),
								  PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());

	const std::optional<PhysicalSegmentRelocation> relocation = codebooks.relocateExtent(
			codebookRequest.codebook_id, VectorCodebookExtentIdCodec::manifestExtentId(codebookRequest.codebook_id),
			PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(4));
	ASSERT_TRUE(relocation.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findExtentLocation(snapshot,
										   VectorCodebookExtentIdCodec::manifestExtentId(codebookRequest.codebook_id))
					  .generation,
			  Generation(2));
	EXPECT_EQ(manifests.read(snapshot, expected.index_id).manifest(), expected);
	EXPECT_EQ(codebooks.read(snapshot, codebookRequest.codebook_id).header.generation, codebookRequest.generation);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsInvalidAllocationIdentityAndReportsExhaustedPhysicalRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest first = manifest();
	VectorManifestStore store(io, *metadata);
	SharedPagePoolAllocationRequest invalidRequest = request(first);
	invalidRequest.page_owner_id = 18;
	EXPECT_THROW(static_cast<void>(store.allocate(invalidRequest, VectorManifestPage::create(owner(first), first),
												  PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))),
				 FormatException);
	ASSERT_TRUE(store.allocate(request(first), VectorManifestPage::create(owner(first), first), PhysicalSlotId(40),
							   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const VectorManifest second = manifest(18, PageId(102), 3);
	EXPECT_FALSE(store.allocate(request(second, SegmentId(72)), VectorManifestPage::create(owner(second), second),
								PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(3))
						 .has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findVectorManifestOptional(snapshot, second.index_id).has_value());
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsInvalidOrUnknownManifestEvacuationRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest source = manifest();
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(source), VectorManifestPage::create(owner(source), source), PhysicalSlotId(40),
							   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	expectFormatError([&] { static_cast<void>(store.evacuate(0, SegmentId(72), LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuate(source.index_id, {}, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePage({}, SegmentId(72), LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePage(PageId(999), SegmentId(72), LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.evacuate(999, SegmentId(72), LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, source.index_id).manifest(), source);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsCorruptDirectoryIdentitiesAndIncompatibleEvacuationPools) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifestStore store(io, *metadata);
	const VectorManifest mapped = manifest(17, PageId(101), 2);
	ASSERT_TRUE(store.allocate(request(mapped, SegmentId(71)), VectorManifestPage::create(owner(mapped), mapped),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());

	// A valid frame with a different logical index must never be trusted merely
	// because its Page Directory location is current.
	VectorManifest mismatched = mapped;
	mismatched.index_id = 18;
	const PageOwnerHeader mismatchedOwner = owner(mismatched);
	overwriteManifestFrame(io, *metadata, mapped.page_id, mismatchedOwner,
						   VectorManifestPage::create(mismatchedOwner, mismatched));
	MetadataSnapshotPin corrupted = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(corrupted, mapped.index_id)); },
					  FormatError::FERR_INVALID_STATE);
	corrupted.release();
	overwriteManifestFrame(io, *metadata, mapped.page_id, owner(mapped),
						   VectorManifestPage::create(owner(mapped), mapped));

	// Materialize an orphaned but internally valid manifest frame. The durable
	// Manifest Directory points to the newer page, so page-based evacuation must
	// reject the stale reverse lookup rather than moving it.
	const VectorManifest detached = manifest(18, PageId(102), 3);
	SharedPagePoolAllocator pools(io, *metadata);
	SharedPagePoolAllocationRequest detachedRequest = request(detached, SegmentId(72));
	const VectorManifestPage detachedPage = VectorManifestPage::create(owner(detached), detached);
	detachedRequest.payload = detachedPage.serialize();
	ASSERT_TRUE(pools.allocateNewPoolPage(detachedRequest, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	const VectorManifest current = manifest(18, PageId(103), 4);
	ASSERT_TRUE(store.allocate(request(current, SegmentId(73)), VectorManifestPage::create(owner(current), current),
							   PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(4))
						.has_value());
	expectFormatError(
			[&] { static_cast<void>(store.evacuatePage(detached.page_id, SegmentId(73), LogSequenceNumber(5))); },
			FormatError::FERR_INVALID_STATE);

	VectorPageStore vectors(io, *metadata);
	const SharedPagePoolAllocationRequest rawRequest = rawVectorRequest(SegmentId(74), PageId(201));
	ASSERT_TRUE(vectors.allocate(rawRequest, rawVectorPage(rawRequest, LogSequenceNumber(6)), PhysicalSlotId(43),
								 PhysicalSlotId(43), LogSequenceNumber(6))
						.has_value());
	expectFormatError(
			[&] { static_cast<void>(store.evacuate(mapped.index_id, rawRequest.segment_id, LogSequenceNumber(7))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.evacuate(mapped.index_id, rawRequest.segment_id, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePage(mapped.page_id, {}, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePage(mapped.page_id, rawRequest.segment_id, {})); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorManifestStoreTest, EvacuatesManifestAndRebindsBothDirectoriesAtomically) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest source = manifest(17, PageId(101), 2);
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(source, SegmentId(71)), VectorManifestPage::create(owner(source), source),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	VectorManifest destination = manifest(18, PageId(102), 3);
	destination.generation = Generation(2);
	ASSERT_TRUE(store.allocate(request(destination, SegmentId(72)),
							   VectorManifestPage::create(owner(destination), destination), PhysicalSlotId(41),
							   PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const SharedPagePoolAllocation relocation = store.evacuate(source.index_id, SegmentId(72), LogSequenceNumber(4));
	EXPECT_EQ(relocation.location.segment_id, SegmentId(72));
	EXPECT_EQ(relocation.location.generation, Generation(2));
	EXPECT_EQ(store.read(oldSnapshot, source.index_id).manifest(), source);
	EXPECT_EQ(metadata->findVectorManifest(oldSnapshot, source.index_id),
			  (VectorManifestLocation{source.index_id, source.page_id, source.generation}));

	MetadataSnapshotPin current = metadata->openSnapshot();
	const VectorManifest expected = [&] {
		VectorManifest result = source;
		result.generation = Generation(2);
		result.committed_lsn = LogSequenceNumber(4);
		return result;
	}();
	EXPECT_EQ(metadata->findVectorManifest(current, source.index_id),
			  (VectorManifestLocation{source.index_id, source.page_id, Generation(2)}));
	EXPECT_EQ(metadata->findPageLocation(current, source.page_id).segment_id, SegmentId(72));
	EXPECT_EQ(store.read(current, source.index_id).manifest(), expected);
	oldSnapshot.release();
	current.release();
	metadata.reset();

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	VectorManifestStore reopenedStore(io, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findVectorManifest(reopened, source.index_id),
			  (VectorManifestLocation{source.index_id, source.page_id, Generation(2)}));
	EXPECT_EQ(reopenedStore.read(reopened, source.index_id).manifest(), expected);
}

TEST_F(RelocatableVectorManifestStoreTest, ReplacementRetainsOldManifestForPinnedSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest original = manifest();
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(original), VectorManifestPage::create(owner(original), original),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	VectorManifest replacement = original;
	replacement.committed_lsn = LogSequenceNumber(3);
	replacement.entries = {{PageId(201), VectorStorageKind::VSK_RAW, 7},
						   {PageId(203), VectorStorageKind::VSK_RAW, 2},
						   {PageId(202), VectorStorageKind::VSK_PQ, 5}};
	const SharedPagePoolAllocation published =
			store.replace(VectorManifestPage::create(owner(replacement), replacement), SegmentId(71),
						  LogSequenceNumber(2), LogSequenceNumber(3));
	EXPECT_EQ(published.location.page_id, original.page_id);
	EXPECT_NE(published.location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(store.read(oldSnapshot, original.index_id).manifest(), original);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.read(current, original.index_id).manifest(), replacement);
	VectorManifest wrongPage = replacement;
	wrongPage.page_id = PageId(102);
	expectFormatError(
			[&] {
				static_cast<void>(store.replace(VectorManifestPage::create(owner(wrongPage), wrongPage), SegmentId(71),
												LogSequenceNumber(3), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	VectorManifest staleAttempt = replacement;
	staleAttempt.committed_lsn = LogSequenceNumber(4);
	expectFormatError(
			[&] {
				static_cast<void>(store.replace(VectorManifestPage::create(owner(staleAttempt), staleAttempt),
												SegmentId(71), LogSequenceNumber(2), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, ReplacementDiskFullRetainsBothPublishedDirectoriesAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const VectorManifest original = manifest();
		VectorManifest replacement = original;
		replacement.committed_lsn = LogSequenceNumber(3U);
		replacement.entries.front().record_count = 7U;
		{
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorManifestStore store(nativeIo, *metadata);
			ASSERT_TRUE(store.allocate(request(original), VectorManifestPage::create(owner(original), original),
									   PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
								.has_value());
			const uint64_t candidateOffset = manifestFrameOffset(*metadata, original.page_id) + RSS_PAGE_SIZE;
			bool diskFullObserved = false;
			{
				StorageIO::ScopedNativeWriteHookForTesting failCandidate(
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
				EXPECT_THROW(
						static_cast<void>(store.replace(VectorManifestPage::create(owner(replacement), replacement),
														SegmentId(71U), LogSequenceNumber(2U), LogSequenceNumber(3U))),
						std::system_error);
			}
			EXPECT_TRUE(diskFullObserved);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, original.index_id).manifest(), original);
			EXPECT_EQ(metadata->findVectorManifest(current, original.index_id),
					  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
			EXPECT_EQ(metadata->findPageLocation(current, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
		}

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		VectorManifestStore recoveredStore(nativeIo, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredStore.read(recovered, original.index_id).manifest(), original);
		EXPECT_EQ(recoveredMetadata->findVectorManifest(recovered, original.index_id),
				  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
		EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, original.page_id).page_index,
				  RSS_PAGE_POOL_FIRST_DATA_FRAME);
		recovered.release();
		EXPECT_EQ(recoveredStore
						  .replace(VectorManifestPage::create(owner(replacement), replacement), SegmentId(71U),
								   LogSequenceNumber(2U), LogSequenceNumber(3U))
						  .location.page_index,
				  2U);
		MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredStore.read(published, original.index_id).manifest(), replacement);
		EXPECT_EQ(recoveredMetadata->findVectorManifest(published, original.index_id),
				  (VectorManifestLocation{replacement.index_id, replacement.page_id, replacement.generation}));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorManifestStoreTest,
	   ReplacementFinalSyncFailureReportsUncertainDurabilityAndReopensBothPublishedDirectories) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const VectorManifest original = manifest();
		VectorManifest replacement = original;
		replacement.committed_lsn = LogSequenceNumber(3U);
		replacement.entries.front().record_count = 7U;
		{
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorManifestStore store(nativeIo, *metadata);
			ASSERT_TRUE(store.allocate(request(original), VectorManifestPage::create(owner(original), original),
									   PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
								.has_value());
			std::atomic<uint32_t> syncCalls{0U};
			{
				StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence([&syncCalls](file_handle_t fd) {
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
				EXPECT_THROW(
						static_cast<void>(store.replace(VectorManifestPage::create(owner(replacement), replacement),
														SegmentId(71U), LogSequenceNumber(2U), LogSequenceNumber(3U))),
						std::runtime_error);
			}
			EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, original.index_id).manifest(), replacement);
			EXPECT_EQ(metadata->findVectorManifest(current, original.index_id),
					  (VectorManifestLocation{replacement.index_id, replacement.page_id, replacement.generation}));
			EXPECT_EQ(metadata->findPageLocation(current, original.page_id).page_index, 2U);
		}

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		VectorManifestStore recoveredStore(nativeIo, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredStore.read(recovered, original.index_id).manifest(), replacement);
		EXPECT_EQ(recoveredMetadata->findVectorManifest(recovered, original.index_id),
				  (VectorManifestLocation{replacement.index_id, replacement.page_id, replacement.generation}));
		EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, original.page_id).page_index, 2U);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorManifestStoreTest,
	   ReplacementPrePublicationSyncFailureRetainsBothPublishedDirectoriesAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const VectorManifest original = manifest();
		VectorManifest replacement = original;
		replacement.committed_lsn = LogSequenceNumber(3U);
		replacement.entries.front().record_count = 7U;
		{
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorManifestStore store(nativeIo, *metadata);
			ASSERT_TRUE(store.allocate(request(original), VectorManifestPage::create(owner(original), original),
									   PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
								.has_value());
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
				EXPECT_THROW(
						static_cast<void>(store.replace(VectorManifestPage::create(owner(replacement), replacement),
														SegmentId(71U), LogSequenceNumber(2U), LogSequenceNumber(3U))),
						std::runtime_error);
			}
			EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, original.index_id).manifest(), original);
			EXPECT_EQ(metadata->findVectorManifest(current, original.index_id),
					  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
			EXPECT_EQ(metadata->findPageLocation(current, original.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
		}

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		VectorManifestStore recoveredStore(nativeIo, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredStore.read(recovered, original.index_id).manifest(), original);
		EXPECT_EQ(recoveredMetadata->findVectorManifest(recovered, original.index_id),
				  (VectorManifestLocation{original.index_id, original.page_id, original.generation}));
		EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, original.page_id).page_index,
				  RSS_PAGE_POOL_FIRST_DATA_FRAME);
		recovered.release();
		EXPECT_EQ(recoveredStore
						  .replace(VectorManifestPage::create(owner(replacement), replacement), SegmentId(71U),
								   LogSequenceNumber(2U), LogSequenceNumber(3U))
						  .location.page_index,
				  2U);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorManifestStoreTest, ReplacementRecoversTheWholeManifestAtWalBoundaries) {
	const VectorManifest original = manifest();
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(original), VectorManifestPage::create(owner(original), original),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	VectorManifest replacement = original;
	replacement.committed_lsn = LogSequenceNumber(3);
	replacement.entries.front().record_count = 7;
	const VectorManifestPage replacementPage = VectorManifestPage::create(owner(replacement), replacement);
	MetadataPublishOptions recordInterrupted;
	recordInterrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.replace(replacementPage, SegmentId(71), LogSequenceNumber(2),
												 LogSequenceNumber(3), recordInterrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto afterRecord = RelocatableMetadataStore::open(io);
	VectorManifestStore afterRecordStore(io, *afterRecord);
	MetadataSnapshotPin oldSnapshot = afterRecord->openSnapshot();
	EXPECT_EQ(afterRecordStore.read(oldSnapshot, original.index_id).manifest(), original);
	oldSnapshot.release();

	MetadataPublishOptions headerInterrupted;
	headerInterrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(afterRecordStore.replace(replacementPage, SegmentId(71), LogSequenceNumber(2),
															LogSequenceNumber(3), headerInterrupted)),
				 BootstrapPublishInterrupted);
	afterRecord.reset();
	auto afterHeader = RelocatableMetadataStore::open(io);
	VectorManifestStore afterHeaderStore(io, *afterHeader);
	MetadataSnapshotPin recovered = afterHeader->openSnapshot();
	EXPECT_EQ(afterHeaderStore.read(recovered, original.index_id).manifest(), replacement);
}

TEST_F(RelocatableVectorManifestStoreTest, RejectsManifestDirectoryAliasesAcrossIndexOwners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest first = manifest(17, PageId(101), 2);
	const VectorManifest second = manifest(18, PageId(102), 3);
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(first, SegmentId(71)), VectorManifestPage::create(owner(first), first),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	ASSERT_TRUE(store.allocate(request(second, SegmentId(72)), VectorManifestPage::create(owner(second), second),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	const std::array alias = {VectorManifestLocation{first.index_id, second.page_id, second.generation}};
	expectFormatError([&] { static_cast<void>(metadata->upsertVectorManifests(alias, {}, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, first.index_id).manifest(), first);
	EXPECT_EQ(store.read(snapshot, second.index_id).manifest(), second);
}

TEST_F(RelocatableVectorManifestStoreTest, RetirementRemovesBothMappingsWhilePinnedSnapshotsRetainTheManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const VectorManifest expected = manifest();
	VectorManifestStore store(io, *metadata);
	expectFormatError([&] { static_cast<void>(store.retire(0, LogSequenceNumber(2), LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.retire(expected.index_id, {}, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.retire(expected.index_id, LogSequenceNumber(2), {})); },
					  FormatError::FERR_INVALID_HEADER);
	ASSERT_TRUE(store.allocate(request(expected), VectorManifestPage::create(owner(expected), expected),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(store.retire(expected.index_id, LogSequenceNumber(3), LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.read(oldSnapshot, expected.index_id).manifest(), expected);
	static_cast<void>(store.retire(expected.index_id, LogSequenceNumber(2), LogSequenceNumber(3)));
	EXPECT_EQ(store.read(oldSnapshot, expected.index_id).manifest(), expected);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findVectorManifestOptional(current, expected.index_id).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, expected.page_id).has_value());
	expectFormatError([&] { static_cast<void>(store.read(current, expected.index_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorManifestStoreTest, RetirementRecordDurableBeforeWalHeaderCanBeRetried) {
	const VectorManifest expected = manifest();
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(expected), VectorManifestPage::create(owner(expected), expected),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(
			static_cast<void>(store.retire(expected.index_id, LogSequenceNumber(2), LogSequenceNumber(3), interrupted)),
			BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	VectorManifestStore retryStore(io, *reopened);
	MetadataSnapshotPin beforeRetry = reopened->openSnapshot();
	EXPECT_EQ(retryStore.read(beforeRetry, expected.index_id).manifest(), expected);
	beforeRetry.release();
	static_cast<void>(retryStore.retire(expected.index_id, LogSequenceNumber(2), LogSequenceNumber(3)));
	MetadataSnapshotPin afterRetry = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findVectorManifestOptional(afterRetry, expected.index_id).has_value());
}

TEST_F(RelocatableVectorManifestStoreTest, RetirementWalHeaderDurableRemovesBothMappingsAfterRecovery) {
	const VectorManifest expected = manifest();
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifestStore store(io, *metadata);
	ASSERT_TRUE(store.allocate(request(expected), VectorManifestPage::create(owner(expected), expected),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(
			static_cast<void>(store.retire(expected.index_id, LogSequenceNumber(2), LogSequenceNumber(3), interrupted)),
			BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findVectorManifestOptional(recovered, expected.index_id).has_value());
	EXPECT_FALSE(reopened->findPageLocationOptional(recovered, expected.page_id).has_value());
}
