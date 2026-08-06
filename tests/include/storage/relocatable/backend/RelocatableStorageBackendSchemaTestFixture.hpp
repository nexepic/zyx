/**
 * @file RelocatableStorageBackendSchemaTestFixture.hpp
 * @brief Shared single-file fixture for relocatable schema and Vector backend tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/RelocatableVectorSearch.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7172737475767778ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	template<typename Predicate>
	bool waitUntil(Predicate predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (predicate()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return predicate();
	}

} // namespace

class RelocatableStorageBackendSchemaTest : public ::testing::Test {
protected:
	std::filesystem::path data_path_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path_ = std::filesystem::temp_directory_path() / ("relocatable_backend_schema_" + to_string(uuid));
		initializeBlankDatabase();
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(data_path_, error);
	}

	void initializeBlankDatabase() {
		auto data = std::make_shared<std::fstream>(data_path_,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		static_cast<void>(RelocatableMetadataStore::initializeNew(dataIo, initialSuperblock()));
		dataIo->sync();
		dataIo.reset();
		data->close();
	}

	[[nodiscard]] RelocatableStorageBackendOptions options() const {
		RelocatableStorageBackendOptions result;
		result.data_path = data_path_;
		result.open_mode = OpenMode::OPEN_EXISTING_FILE;
		result.maximum_index_definitions = 32U;
		result.maximum_online_index_entries = 1024U;
		result.maximum_online_index_pages = 128U;
		result.online_index_sort_run_bytes = 4096U;
		result.maximum_online_index_sort_runs = 8U;
		return result;
	}

	[[nodiscard]] VectorIndexStateReclaimOptions vectorStateReclaimOptions() const {
		VectorIndexStateReclaimOptions result;
		result.manifests = {32U, 32U};
		result.raw = {32U, 32U, 64U, 256U, 4096U, 256U};
		result.derived = {32U, 4096U, 4096U, 256U, 256U, 64U, 256U};
		result.mapping_trees = {32U, 32U, 32U, 256U, 256U, 256U};
		result.codebooks = {32U, 4096U, 4096U, 256U, 256U};
		result.empty_pools = {32U, 4096U, 4096U, 512U, 512U};
		return result;
	}

	[[nodiscard]] std::vector<BTreePageEntry> readIndexEntries(uint64_t indexId) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("schema test could not reopen its relocatable file");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::vector<BTreePageEntry> result =
				BTreeIndex(indexId, trees, *metadata).scan(snapshot, std::nullopt, 8U);
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
		return result;
	}

	[[nodiscard]] VectorManifest readVectorManifest(uint64_t indexId) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("schema test could not reopen its relocatable vector file");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorManifestStore manifests(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const VectorManifest result = manifests.read(snapshot, indexId).manifest();
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
		return result;
	}

	// Writes a checksummed Manifest payload while retaining its durable page owner.
	// Tests use this only to model an already-persisted torn/corrupt generation.
	void overwriteVectorManifest(const VectorManifest &manifest) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation location = metadata->findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const PageOwnerHeader owner{manifest.page_id,
									manifest.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0U,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									manifest.generation,
									manifest.committed_lsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		dataIo->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		const std::vector<std::byte> bytes = page.serialize();
		dataIo->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	// Invokes the storage-internal boundary with a deliberately unmaterialized
	// Manifest. This reaches fail-closed checks that the on-disk codec rejects
	// earlier, without weakening either layer's independent validation.
	void expectDirectVectorSearchRejected(const IndexDefinition &definition, const VectorManifest &manifest,
										  const LogicalGraphVectorSearchRequest &request,
										  const char *expectedMessage) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		PhysicalSegmentAllocator segments(dataIo, *metadata);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		VectorPageStore vectors(dataIo, *metadata);
		VectorAdjacencyPageStore adjacencies(dataIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		try {
			static_cast<void>(RelocatableVectorSearch::search(snapshot, definition, manifest, request, trees, vectors,
															  adjacencies, codebooks, *metadata));
			FAIL() << "expected RelocatableVectorSearch to reject the inconsistent Manifest";
		} catch (const FormatException &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	// The public backend validates query arguments first, but callers of this
	// storage boundary must receive the same fail-closed validation.
	void expectDirectVectorSearchInvalidArgument(const IndexDefinition &definition, const VectorManifest &manifest,
												 const LogicalGraphVectorSearchRequest &request,
												 const char *expectedMessage) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		PhysicalSegmentAllocator segments(dataIo, *metadata);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		VectorPageStore vectors(dataIo, *metadata);
		VectorAdjacencyPageStore adjacencies(dataIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		try {
			static_cast<void>(RelocatableVectorSearch::search(snapshot, definition, manifest, request, trees, vectors,
															  adjacencies, codebooks, *metadata));
			FAIL() << "expected RelocatableVectorSearch to reject its invalid request";
		} catch (const std::invalid_argument &error) {
			EXPECT_STREQ(error.what(), expectedMessage);
		}
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	[[nodiscard]] std::vector<std::byte> readVectorRecord(uint64_t vectorId) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("schema test could not reopen its relocatable Vector files");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const VectorLocation location = metadata->findVectorLocation(snapshot, vectorId);
		const std::vector<std::byte> result =
				vectors.read(snapshot, location.page_id).read(location.slot_id, location.slot_generation);
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
		return result;
	}

	void overwriteVectorRecord(uint64_t vectorId, std::span<const std::byte> replacement) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const VectorLocation location = metadata->findVectorLocation(snapshot, vectorId);
		VectorPage page = vectors.read(snapshot, location.page_id);
		ASSERT_EQ(replacement.size(), page.header().record_bytes);
		page.overwrite(location.slot_id, location.slot_generation, replacement);
		const PageLocation pageLocation = metadata->findPageLocation(snapshot, location.page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, pageLocation.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(pageLocation.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const std::vector<std::byte> bytes = page.serialize();
		dataIo->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	[[nodiscard]] std::vector<std::byte> readVectorPageRecord(PageId pageId, uint64_t vectorId) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("schema test could not reopen its relocatable Vector files");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const VectorPage page = vectors.read(snapshot, pageId);
		const std::optional<uint16_t> slot = page.findVectorId(vectorId);
		if (!slot.has_value()) {
			throw std::runtime_error("schema test could not find its persisted Vector record");
		}
		const std::vector<std::byte> result = page.read(*slot);
		snapshot.release();
		metadata.reset();
		dataIo.reset();
		data->close();
		return result;
	}

	void overwriteVectorPageRecord(PageId pageId, uint64_t vectorId, std::span<const std::byte> replacement) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		VectorPage page = vectors.read(snapshot, pageId);
		const std::optional<uint16_t> slot = page.findVectorId(vectorId);
		ASSERT_TRUE(slot.has_value());
		ASSERT_EQ(replacement.size(), page.header().record_bytes);
		page.overwrite(*slot, replacement);
		const PageLocation location = metadata->findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const std::vector<std::byte> bytes = page.serialize();
		dataIo->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	void overwriteBTreeLeafEntry(uint64_t indexId, std::span<const std::byte> targetKey,
								 BTreePageEntry replacement) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, indexId);
		BTreePage original = trees.read(snapshot, root.root_page_id);
		BTreePageHeader header = original.header();
		ASSERT_EQ(header.kind, BTreeNodeKind::BPN_LEAF);
		const std::vector<BTreePageEntry> entries = original.entries();
		header.entry_count = 0U;
		const PageOwnerHeader owner{header.page_id,
									header.index_id,
									PageOwnerKind::POK_INDEX,
									0U,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									header.generation,
									original.slottedPage().ownerCommittedLsn()};
		BTreePage rewritten = BTreePage::create(owner, header);
		bool replaced = false;
		for (const BTreePageEntry &entry: entries) {
			if (std::ranges::equal(entry.key, targetKey)) {
				rewritten.insert(std::move(replacement));
				replaced = true;
			} else {
				rewritten.insert(entry);
			}
		}
		ASSERT_TRUE(replaced);
		const PageLocation location = metadata->findPageLocation(snapshot, root.root_page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const std::vector<std::byte> bytes = rewritten.serialize();
		dataIo->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	void publishVectorAdjacency(uint64_t indexId, std::span<const CanonicalVectorRecord> records,
								uint16_t maximumDegree = 1U) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		VectorAdjacencyPageStore adjacencies(dataIo, *metadata);
		VectorManifestStore manifests(dataIo, *metadata);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		OfflineVectorAdjacencyRebuildOptions rebuild;
		rebuild.maximum_exact_records = records.size();
		rebuild.maximum_degree = maximumDegree;
		rebuild.adjacency_generation = Generation(1U);
		rebuild.pages = {PhysicalSlotId(128U), PhysicalSlotId(129U), records.size(), 8U};
		rebuild.mapping_index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD1);
		rebuild.mapping.tree = {PhysicalSlotId(130U), PhysicalSlotId(131U), rebuild.mapping_index_id, records.size(),
								8U};
		rebuild.mapping.maximum_entries = records.size();
		static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, indexId,
															 records, rebuild));
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	void publishVectorPq(uint64_t indexId, std::span<const CanonicalVectorRecord> records) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorPageStore vectors(dataIo, *metadata);
		VectorManifestStore manifests(dataIo, *metadata);
		SlottedPagePool pages(dataIo, *metadata);
		BTreePageStore trees(pages, *metadata);
		PhysicalSegmentAllocator segments(dataIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);

		std::vector<DeterministicPqTrainingRecord> training;
		training.reserve(records.size());
		for (const CanonicalVectorRecord &record: records) {
			training.push_back({record.vector_id, record.raw_bfloat16});
		}
		ASSERT_FALSE(training.empty());
		ASSERT_EQ(training.front().raw_bfloat16.size() % sizeof(uint16_t), 0U);
		const uint32_t dimensions = static_cast<uint32_t>(training.front().raw_bfloat16.size() / sizeof(uint16_t));
		const uint32_t centroidCount = static_cast<uint32_t>(training.size());
		OfflineVectorPqRebuildOptions rebuild;
		rebuild.training = {dimensions, dimensions, centroidCount, 3U, training.size(), training.size()};
		rebuild.codebook.codebook_id = 0xE1U;
		rebuild.codebook.manifest_segment_id = SegmentId(0xE1U);
		rebuild.codebook.generation = Generation(1U);
		rebuild.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		rebuild.codebook.dimensions = dimensions;
		rebuild.codebook.subspace_count = dimensions;
		rebuild.codebook.centroid_count = centroidCount;
		rebuild.codebook_first_slot = PhysicalSlotId(132U);
		rebuild.codebook_last_slot = PhysicalSlotId(132U);
		rebuild.pages = {PhysicalSlotId(133U), PhysicalSlotId(134U), Generation(1U), records.size(), 8U};
		rebuild.mapping_index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xD2);
		rebuild.mapping.tree = {PhysicalSlotId(135U), PhysicalSlotId(139U), rebuild.mapping_index_id, records.size(),
								8U};
		rebuild.mapping.maximum_entries = records.size();
		static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, indexId,
													  training, rebuild));
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}

	void advanceManifestCodebookGeneration(uint64_t indexId) const {
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		VectorManifestStore manifests(dataIo, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		VectorManifest manifest = manifests.read(snapshot, indexId).manifest();
		const PageLocation location = metadata->findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		manifest.codebook_generation = Generation(manifest.codebook_generation.value() + 1U);
		const PageOwnerHeader owner{manifest.page_id,
									manifest.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0U,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									manifest.generation,
									manifest.committed_lsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		dataIo->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		dataIo->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, page.serialize().data(), page.serialize().size());
		dataIo->sync();
		metadata.reset();
		dataIo.reset();
		data->close();
	}
};
