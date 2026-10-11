/**
 * @file test_OfflineVectorBaseRebuild.cpp
 * @brief Atomic PQ, adjacency, and delta-folding Vector base-rebuild tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	constexpr uint64_t PQ_MAPPING_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A1);
	constexpr uint64_t ADJACENCY_MAPPING_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A1);

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> result(values.size() * sizeof(uint16_t));
		size_t ordinal = 0U;
		for (const float value: values) {
			LittleEndianCodec::writeU16(result, ordinal * sizeof(uint16_t),
									static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			++ordinal;
		}
		return result;
	}

	std::vector<DeterministicPqTrainingRecord> trainingRecords() {
		return {{1U, bfloat16({0.0F, 0.0F, 10.0F, 10.0F})},
				{2U, bfloat16({0.0F, 1.0F, 9.0F, 10.0F})},
				{3U, bfloat16({9.0F, 10.0F, 0.0F, 0.0F})},
				{4U, bfloat16({10.0F, 9.0F, 1.0F, 0.0F})},
				{5U, bfloat16({5.0F, 5.0F, 5.0F, 5.0F})}};
	}

	std::vector<DeterministicPqTrainingRecord> trainingRecords(size_t count) {
		std::vector<DeterministicPqTrainingRecord> result;
		result.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			const float value = static_cast<float>(ordinal + 1U);
			result.push_back({static_cast<uint64_t>(ordinal + 1U),
						  bfloat16({value, value * 0.5F, value * 0.25F, value * 0.125F})});
		}
		return result;
	}

	RawVectorMaterializationRecord rawRecord(const DeterministicPqTrainingRecord &record) {
		RawVectorMaterializationRecord result;
		result.vector_id = record.vector_id;
		result.bytes.resize(sizeof(uint64_t) + record.raw_bfloat16.size());
		LittleEndianCodec::writeU64(result.bytes, 0U, record.vector_id);
		std::copy(record.raw_bfloat16.begin(), record.raw_bfloat16.end(), result.bytes.begin() + sizeof(uint64_t));
		return result;
	}

	OfflineVectorBaseRebuildOptions rebuildOptions(uint64_t codebookId = 71U,
													 Generation adjacencyGeneration = Generation(1U)) {
		OfflineVectorBaseRebuildOptions result;
		result.pq.training = {4U, 2U, 2U, 3U, 16U, 5U};
		result.pq.codebook.codebook_id = codebookId;
		result.pq.codebook.manifest_segment_id = SegmentId(codebookId);
		result.pq.codebook.generation = Generation(1U);
		result.pq.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.pq.codebook.dimensions = result.pq.training.dimensions;
		result.pq.codebook.subspace_count = result.pq.training.subspace_count;
		result.pq.codebook.centroid_count = result.pq.training.centroid_count;
		result.pq.codebook_first_slot = PhysicalSlotId(50U);
		result.pq.codebook_last_slot = PhysicalSlotId(50U);
		result.pq.pages = {PhysicalSlotId(51U), PhysicalSlotId(55U), Generation(1U), 16U, 4U};
		result.pq.mapping_index_id = PQ_MAPPING_ID;
		result.pq.mapping.tree = {PhysicalSlotId(56U), PhysicalSlotId(60U), PQ_MAPPING_ID, 16U, 4U};
		result.pq.mapping.maximum_entries = 16U;
		result.adjacency.maximum_exact_records = 16U;
		result.adjacency.maximum_degree = 2U;
		result.adjacency.adjacency_generation = adjacencyGeneration;
		result.adjacency.pages = {PhysicalSlotId(61U), PhysicalSlotId(65U), 16U, 4U};
		result.adjacency.mapping_index_id = ADJACENCY_MAPPING_ID;
		result.adjacency.mapping.tree = {PhysicalSlotId(66U), PhysicalSlotId(70U), ADJACENCY_MAPPING_ID, 16U, 4U};
		result.adjacency.mapping.maximum_entries = 16U;
		result.maximum_delta_mapping_pages = 4U;
		result.maximum_delta_overlay_entries = 16U;
		return result;
	}

	void overwriteManifest(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
								   const VectorManifest &manifest) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
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
		io->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, page.serialize().data(), page.serialize().size());
	}

	void replaceManifest(VectorManifestStore &manifests, RelocatableMetadataStore &metadata,
						 VectorManifest manifest) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		ASSERT_LT(snapshot.superblock().checkpoint_lsn.value(), std::numeric_limits<uint64_t>::max());
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		const LogSequenceNumber expectedOwnerLsn = manifest.committed_lsn;
		snapshot.release();
		manifest.committed_lsn = checkpointLsn;
		const PageOwnerHeader owner{manifest.page_id,
								manifest.index_id,
								PageOwnerKind::POK_VECTOR_MANIFEST,
								0U,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								manifest.generation,
								checkpointLsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
		static_cast<void>(manifests.replace(page, pool.segment_id, expectedOwnerLsn, checkpointLsn));
	}

	class ThrowAfterCancellationChecks final : public CancellationCheckpoint {
	public:
		explicit ThrowAfterCancellationChecks(size_t throwOnCheck) : throw_on_check_(throwOnCheck) {}

		void check() const override {
			++check_count_;
			if (check_count_ >= throw_on_check_) {
				throw std::runtime_error("test cancellation");
			}
		}

		[[nodiscard]] size_t checkCount() const { return check_count_; }

	private:
		size_t throw_on_check_ = 0U;
		mutable size_t check_count_ = 0U;
	};

} // namespace

class RelocatableOfflineVectorBaseRebuildTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		path = std::filesystem::temp_directory_path() /
				("relocatable_offline_base_rebuild_" + to_string(boost::uuids::random_generator()()));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
	}

	void materializeRawAndManifest(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
											 VectorManifestStore &manifests,
											 VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2) {
		const std::vector<DeterministicPqTrainingRecord> records = trainingRecords();
		materializeRawAndManifest(metadata, vectors, manifests, records, metric);
	}

	void materializeRawAndManifest(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
											 VectorManifestStore &manifests,
											 std::span<const DeterministicPqTrainingRecord> records,
											 VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2) {
		std::vector<RawVectorMaterializationRecord> raw;
		raw.reserve(records.size());
		for (const DeterministicPqTrainingRecord &record: records) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(
				17U, raw,
				{PhysicalSlotId(40U), PhysicalSlotId(40U), raw.size(), RSS_PAGE_POOL_DATA_FRAME_COUNT},
				LogSequenceNumber(2U)));
		VectorManifest manifest;
		manifest.index_id = 17U;
		manifest.dimensions = 4U;
		manifest.metric = metric;
		static_cast<void>(
				manifests.materialize(manifest, PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(3U)));
	}

	void overwriteBTreeLeafValue(RelocatableMetadataStore &metadata, BTreePageStore &trees, uint64_t indexId,
								 std::span<const std::byte> targetKey, uint64_t replacementValue) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
		const BTreePage original = trees.read(snapshot, root.root_page_id);
		ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
		BTreePageHeader header = original.header();
		header.entry_count = 0U;
		const PageOwnerHeader owner{header.page_id, header.index_id, PageOwnerKind::POK_INDEX, 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation,
									 original.slottedPage().ownerCommittedLsn()};
		BTreePage replacement = BTreePage::create(owner, header);
		bool replaced = false;
		for (const BTreePageEntry &entry: original.entries()) {
			if (std::ranges::equal(entry.key, targetKey)) {
				replacement.insert({entry.key, replacementValue});
				replaced = true;
			} else {
				replacement.insert(entry);
			}
		}
		ASSERT_TRUE(replaced);
		const PageLocation location = metadata.findPageLocation(snapshot, root.root_page_id);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																					pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const std::vector<std::byte> bytes = replacement.serialize();
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		io->sync();
	}

	void overwriteBTreeLeafEntry(RelocatableMetadataStore &metadata, BTreePageStore &trees, uint64_t indexId,
								 std::span<const std::byte> targetKey, BTreePageEntry replacementEntry) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
		const BTreePage original = trees.read(snapshot, root.root_page_id);
		ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
		BTreePageHeader header = original.header();
		header.entry_count = 0U;
		const PageOwnerHeader owner{header.page_id, header.index_id, PageOwnerKind::POK_INDEX, 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation,
								 original.slottedPage().ownerCommittedLsn()};
		BTreePage replacement = BTreePage::create(owner, header);
		bool replaced = false;
		for (const BTreePageEntry &entry: original.entries()) {
			if (std::ranges::equal(entry.key, targetKey)) {
				replacement.insert(std::move(replacementEntry));
				replaced = true;
			} else {
				replacement.insert(entry);
			}
		}
		ASSERT_TRUE(replaced);
		ASSERT_LT(snapshot.superblock().checkpoint_lsn.value(), std::numeric_limits<uint64_t>::max());
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		static_cast<void>(trees.replace(replacement, checkpointLsn));
	}

	void eraseBTreeLeafEntry(RelocatableMetadataStore &metadata, BTreePageStore &trees, uint64_t indexId,
								std::span<const std::byte> targetKey) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
		const BTreePage original = trees.read(snapshot, root.root_page_id);
		ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
		BTreePageHeader header = original.header();
		header.entry_count = 0U;
		const PageOwnerHeader owner{header.page_id, header.index_id, PageOwnerKind::POK_INDEX, 0U,
								 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation,
								 original.slottedPage().ownerCommittedLsn()};
		BTreePage replacement = BTreePage::create(owner, header);
		bool erased = false;
		for (const BTreePageEntry &entry: original.entries()) {
			if (std::ranges::equal(entry.key, targetKey)) {
				erased = true;
			} else {
				replacement.insert(entry);
			}
		}
		ASSERT_TRUE(erased);
		ASSERT_LT(snapshot.superblock().checkpoint_lsn.value(), std::numeric_limits<uint64_t>::max());
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		static_cast<void>(trees.replace(replacement, checkpointLsn));
	}

	void expectPqMappingKeyRejectedBeforeReplacement(std::vector<std::byte> replacementKey,
											 FormatError expectedError) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(io, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

		MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
		const std::vector<BTreePageEntry> mappings =
				BTreeIndex(PQ_MAPPING_ID, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
		mappingSnapshot.release();
		ASSERT_FALSE(mappings.empty());
		overwriteBTreeLeafEntry(*metadata, trees, PQ_MAPPING_ID, mappings.front().key,
								{std::move(replacementKey), mappings.front().value_or_child});

		try {
			static_cast<void>(OfflineVectorBaseRebuild::run(
					trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
					rebuildOptions(71U, Generation(2U))));
			FAIL() << "a corrupted PQ mapping key must reject the replacement before publication";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expectedError) << error.what();
		}
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
		EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id,
				  first.manifest.pq_mapping_root_page_id);
		snapshot.release();
	}

	void expectAlreadyPublishedMappingRejected(uint64_t mappingIndexId, bool eraseLastEntry) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(io, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

		MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
		const std::vector<BTreePageEntry> mappings =
				BTreeIndex(mappingIndexId, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
		mappingSnapshot.release();
		ASSERT_FALSE(mappings.empty());
		const BTreePageEntry &last = mappings.back();
		if (eraseLastEntry) {
			eraseBTreeLeafEntry(*metadata, trees, mappingIndexId, last.key);
		} else {
			const uint64_t unexpectedVectorId = 6U;
			const std::vector<std::byte> unexpectedKey = mappingIndexId == PQ_MAPPING_ID
															 ? VectorPqMapping::key(unexpectedVectorId)
															 : VectorAdjacencyMapping::key(unexpectedVectorId);
			overwriteBTreeLeafEntry(*metadata, trees, mappingIndexId, last.key,
											{unexpectedKey, last.value_or_child});
		}

		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
					 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions())),
				 FormatException);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
		snapshot.release();
	}

	void expectForeignDerivedPageRejected(uint64_t mappingIndexId) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(io, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		ASSERT_LT(snapshot.superblock().checkpoint_lsn.value(), std::numeric_limits<uint64_t>::max());
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		const std::vector<BTreePageEntry> mappings =
				BTreeIndex(mappingIndexId, trees, *metadata).scan(snapshot, std::nullopt, 16U);
		snapshot.release();
		ASSERT_FALSE(mappings.empty());

		PageId foreignPage;
		if (mappingIndexId == PQ_MAPPING_ID) {
			const std::array<PqVectorMaterializationRecord, 1U> foreignRecords{{{1U, {0U, 1U}}}};
			const VectorPqMaterializationResult foreign = vectors.materializePq(
					18U, foreignRecords,
					{PhysicalSlotId(71U), PhysicalSlotId(71U), Generation(1U), 1U, 1U}, checkpointLsn);
			ASSERT_EQ(foreign.pages.size(), 1U);
			foreignPage = foreign.pages.front().page_id;
		} else {
			const std::array<VectorAdjacencyMaterializationRecord, 1U> foreignRecords{{{1U, {2U}}}};
			const VectorAdjacencyMaterializationResult foreign = adjacencies.materialize(
					18U, foreignRecords, {PhysicalSlotId(71U), PhysicalSlotId(71U), 1U, 1U}, checkpointLsn);
			ASSERT_EQ(foreign.pages.size(), 1U);
			foreignPage = foreign.pages.front().page_id;
		}
		overwriteBTreeLeafValue(*metadata, trees, mappingIndexId, mappings.front().key, foreignPage.value());

		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
					 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
					 rebuildOptions(71U, Generation(2U)))),
				 FormatException);
		snapshot = metadata->openSnapshot();
		EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
		snapshot.release();
	}

	void expectDanglingMappingRootRejected(uint64_t mappingIndexId) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(io, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		ASSERT_TRUE(metadata->findIndexRootOptional(snapshot, mappingIndexId).has_value());
		ASSERT_LT(snapshot.superblock().checkpoint_lsn.value(), std::numeric_limits<uint64_t>::max());
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		const std::array<IndexRootLocation, 0U> noRoots{};
		const std::array<uint64_t, 1U> erasedRootIds{mappingIndexId};
		static_cast<void>(trees.publishRoots(noRoots, erasedRootIds, checkpointLsn));

		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
					 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
					 rebuildOptions(71U, Generation(2U)))),
				 FormatException);
		snapshot = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, mappingIndexId).has_value());
		EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
		snapshot.release();
	}

	void overwritePagePayload(RelocatableMetadataStore &metadata, PageId pageId, std::span<const std::byte> bytes) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
										static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
		snapshot.release();
		io->writeAt(offset, bytes.data(), bytes.size());
		io->sync();
	}

	void expectAlreadyPublishedPayloadRejected(bool pq) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(io, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::optional<PageId> pageId = pq ? VectorPqMapping::findPage(snapshot, trees, *metadata, PQ_MAPPING_ID, 1U)
															 : VectorAdjacencyMapping::findPage(snapshot, trees, *metadata,
																						 ADJACENCY_MAPPING_ID, 1U);
		ASSERT_TRUE(pageId.has_value());
		if (pq) {
			VectorPage page = vectors.read(snapshot, *pageId);
			const std::optional<uint16_t> slot = page.findVectorId(1U);
			ASSERT_TRUE(slot.has_value());
			std::vector<std::byte> record = page.read(*slot);
			record.back() ^= std::byte{0x01};
			page.overwrite(*slot, record);
			snapshot.release();
			overwritePagePayload(*metadata, *pageId, page.serialize());
		} else {
			SlottedPage page = adjacencies.readPage(snapshot, *pageId);
			std::optional<SlottedSlotReference> record;
			for (const SlottedSlotReference &slot: page.liveSlots()) {
				if (VectorAdjacencyCodec::decode(page.read(slot)).vector_id == 1U) {
					record = slot;
					break;
				}
			}
			ASSERT_TRUE(record.has_value());
			page.overwrite(*record, VectorAdjacencyCodec::encode({1U, {3U}}));
			snapshot.release();
			overwritePagePayload(*metadata, *pageId, page.serialize());
		}
		auto reopenedMetadata = RelocatableMetadataStore::open(io);
		VectorPageStore reopenedVectors(io, *reopenedMetadata);
		VectorAdjacencyPageStore reopenedAdjacencies(io, *reopenedMetadata);
		VectorManifestStore reopenedManifests(io, *reopenedMetadata);
		SlottedPagePool reopenedSlotted(io, *reopenedMetadata);
		BTreePageStore reopenedTrees(reopenedSlotted, *reopenedMetadata);
		PhysicalSegmentAllocator reopenedSegments(io, *reopenedMetadata);
		VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopenedMetadata);
		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
					 reopenedTrees, reopenedVectors, reopenedAdjacencies, reopenedCodebooks, reopenedManifests, *reopenedMetadata,
					 17U, trainingRecords(), rebuildOptions())),
				 FormatException);
		snapshot = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedManifests.read(snapshot, 17U).manifest(), first.manifest);
		snapshot.release();
	}

};

TEST_F(RelocatableOfflineVectorBaseRebuildTest, PublishesOneCompletePqAndAdjacencyBaseGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorBaseRebuildOptions options = rebuildOptions();
	const OfflineVectorBaseRebuildResult result = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), options);
	EXPECT_FALSE(result.already_published);
	EXPECT_FALSE(result.reused_codebook);
	EXPECT_EQ(result.vector_count, trainingRecords().size());
	EXPECT_EQ(result.manifest.pq_mapping_index_id, PQ_MAPPING_ID);
	EXPECT_TRUE(result.manifest.pq_mapping_root_page_id.isValid());
	EXPECT_EQ(result.manifest.adjacency_mapping_index_id, ADJACENCY_MAPPING_ID);
	EXPECT_TRUE(result.manifest.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(result.manifest.adjacency_generation, Generation(1U));
	EXPECT_EQ(result.manifest.delta_mapping_index_id, 0U);
	EXPECT_FALSE(result.manifest.delta_mapping_root_page_id.isValid());
	EXPECT_EQ(result.manifest.delta_entry_count, 0U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), result.manifest);
	ASSERT_TRUE(VectorPqMapping::isMappingIndexId(PQ_MAPPING_ID));
	ASSERT_TRUE(VectorPqMapping::isMappingIndexId(result.manifest.pq_mapping_index_id));
	const std::optional<PageId> pqPage = VectorPqMapping::findPage(snapshot, trees, *metadata, PQ_MAPPING_ID, 3U);
	ASSERT_TRUE(pqPage.has_value());
	EXPECT_TRUE(vectors.read(snapshot, *pqPage).findVectorId(3U).has_value());
	const std::optional<PageId> adjacencyPage =
			VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, ADJACENCY_MAPPING_ID, 3U);
	ASSERT_TRUE(adjacencyPage.has_value());
	EXPECT_EQ(adjacencies.readRecord(snapshot, *adjacencyPage, 3U).vector_id, 3U);
	snapshot.release();

	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), options);
	EXPECT_TRUE(retry.already_published);
	EXPECT_TRUE(retry.reused_codebook);
	EXPECT_EQ(retry.manifest, result.manifest);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, PublishesBaseGenerationWithCosineMetric) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests, VectorDistanceMetric::VDM_COSINE);

	const OfflineVectorBaseRebuildResult result = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	EXPECT_EQ(result.manifest.metric, VectorDistanceMetric::VDM_COSINE);
	EXPECT_TRUE(result.manifest.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(result.manifest.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, PublishesBaseGenerationWithInnerProductMetric) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests, VectorDistanceMetric::VDM_INNER_PRODUCT);

	const OfflineVectorBaseRebuildResult result = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	EXPECT_EQ(result.manifest.metric, VectorDistanceMetric::VDM_INNER_PRODUCT);
	EXPECT_TRUE(result.manifest.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(result.manifest.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsPqPageBudgetBeforePublishingAnyDerivedGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const std::vector<DeterministicPqTrainingRecord> records = trainingRecords(300U);
	materializeRawAndManifest(*metadata, vectors, manifests, records);

	OfflineVectorBaseRebuildOptions options = rebuildOptions();
	options.pq.training.maximum_records = records.size();
	options.pq.training.maximum_samples = 16U;
	options.pq.pages.maximum_records = records.size();
	options.pq.pages.maximum_pages = 1U;
	options.pq.mapping.maximum_entries = records.size();
	options.pq.mapping.tree.maximum_entries = records.size();
	options.adjacency.build_mode = VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
	options.adjacency.maximum_candidate_records = records.size();
	options.adjacency.candidate_search_budget = 16U;
	options.adjacency.pages.maximum_records = records.size();
	options.adjacency.pages.maximum_pages = 32U;
	options.adjacency.mapping.maximum_entries = records.size();
	options.adjacency.mapping.tree.maximum_entries = records.size();

	try {
		static_cast<void>(OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, records, options));
		FAIL() << "the derived PQ page plan must honor its page budget";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
		EXPECT_NE(std::string(error.what()).find("PQ page bound"), std::string::npos);
	}
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findExtentLocationOptional(
				 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(options.pq.codebook.codebook_id))
				 .has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsPqMappingTreeBudgetBeforePublishingAnyDerivedGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const std::vector<DeterministicPqTrainingRecord> records = trainingRecords(2000U);
	materializeRawAndManifest(*metadata, vectors, manifests, records);

	OfflineVectorBaseRebuildOptions options = rebuildOptions();
	options.pq.training.maximum_records = records.size();
	options.pq.training.maximum_samples = 16U;
	options.pq.pages.maximum_records = records.size();
	options.pq.mapping.maximum_entries = records.size();
	options.pq.mapping.tree.maximum_entries = records.size();
	options.pq.mapping.tree.maximum_pages = 1U;
	options.adjacency.build_mode = VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
	options.adjacency.maximum_candidate_records = records.size();
	options.adjacency.candidate_search_budget = 16U;
	options.adjacency.pages.maximum_records = records.size();
	options.adjacency.pages.maximum_pages = 32U;
	options.adjacency.mapping.maximum_entries = records.size();
	options.adjacency.mapping.tree.maximum_entries = records.size();

	try {
		static_cast<void>(OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, records, options));
		FAIL() << "the immutable PQ mapping tree must honor its page budget";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
		EXPECT_NE(std::string(error.what()).find("mapping-tree plan"), std::string::npos) << error.what();
	}
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, ADJACENCY_MAPPING_ID).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, PreservesCanonicalNonPqManifestInventoryAcrossBasePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	MetadataSnapshotPin source = metadata->openSnapshot();
	VectorManifest base = manifests.read(source, 17U).manifest();
	const VectorLocation rawLocation = metadata->findVectorLocation(source, 1U);
	const VectorPage rawPage = vectors.read(source, rawLocation.page_id);
	source.release();
	base.entries.push_back({rawLocation.page_id, VectorStorageKind::VSK_RAW, rawPage.header().live_count});
	replaceManifest(manifests, *metadata, base);

	const OfflineVectorBaseRebuildResult result = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	EXPECT_TRUE(std::ranges::any_of(result.manifest.entries, [rawLocation](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_RAW && entry.page_id == rawLocation.page_id;
	}));
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	EXPECT_TRUE(retry.already_published);
	EXPECT_EQ(retry.manifest, result.manifest);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsAdjacencyMappingThatTargetsARawPageBeforePublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
	const std::vector<BTreePageEntry> mappings =
			BTreeIndex(ADJACENCY_MAPPING_ID, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
	const VectorLocation rawLocation = metadata->findVectorLocation(mappingSnapshot, 1U);
	mappingSnapshot.release();
	ASSERT_FALSE(mappings.empty());
	overwriteBTreeLeafEntry(*metadata, trees, ADJACENCY_MAPPING_ID, mappings.front().key,
						{mappings.front().key, rawLocation.page_id.value()});

	try {
		static_cast<void>(OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				rebuildOptions(71U, Generation(2U))));
		FAIL() << "a raw Vector page must not be decoded as derived adjacency";
	} catch (const FormatException &error) {
		EXPECT_NE(std::string(error.what()).find("invalid current pool or owner identity"), std::string::npos)
				<< error.what();
	}
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedPqMappingWithAMissingEntry) {
	expectAlreadyPublishedMappingRejected(PQ_MAPPING_ID, true);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedAdjacencyMappingWithAMissingEntry) {
	expectAlreadyPublishedMappingRejected(ADJACENCY_MAPPING_ID, true);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedPqMappingWithAnUnexpectedVectorId) {
	expectAlreadyPublishedMappingRejected(PQ_MAPPING_ID, false);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedAdjacencyMappingWithAnUnexpectedVectorId) {
	expectAlreadyPublishedMappingRejected(ADJACENCY_MAPPING_ID, false);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsPqMappingThatTargetsAnotherVectorIndex) {
	expectForeignDerivedPageRejected(PQ_MAPPING_ID);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAdjacencyMappingThatTargetsAnotherVectorIndex) {
	expectForeignDerivedPageRejected(ADJACENCY_MAPPING_ID);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsDanglingPqMappingRoot) {
	expectDanglingMappingRootRejected(PQ_MAPPING_ID);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsDanglingAdjacencyMappingRoot) {
	expectDanglingMappingRootRejected(ADJACENCY_MAPPING_ID);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedPqMappingWithDifferentCodes) {
	expectAlreadyPublishedPayloadRejected(true);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAlreadyPublishedAdjacencyMappingWithDifferentNeighbors) {
	expectAlreadyPublishedPayloadRejected(false);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, CancellationDuringDerivedBuildLeavesThePreviousGenerationVisible) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	MetadataSnapshotPin beforeSnapshot = metadata->openSnapshot();
	const VectorManifest before = manifests.read(beforeSnapshot, 17U).manifest();
	beforeSnapshot.release();
	ThrowAfterCancellationChecks cancellation(8U);
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions(),
				&cancellation)),
			 std::runtime_error);
	EXPECT_GE(cancellation.checkCount(), 8U);

	MetadataSnapshotPin cancelledSnapshot = metadata->openSnapshot();
	const VectorManifest afterCancellation = manifests.read(cancelledSnapshot, 17U).manifest();
	EXPECT_EQ(afterCancellation, before);
	EXPECT_FALSE(metadata->findIndexRootOptional(cancelledSnapshot, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(cancelledSnapshot, ADJACENCY_MAPPING_ID).has_value());
	cancelledSnapshot.release();

	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	EXPECT_FALSE(retry.already_published);
	EXPECT_TRUE(retry.manifest.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(retry.manifest.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, NativePrePublicationSyncFailureRetainsOldGenerationAndCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorPageStore vectors(nativeIo, *metadata);
		VectorAdjacencyPageStore adjacencies(nativeIo, *metadata);
		VectorManifestStore manifests(nativeIo, *metadata);
		SlottedPagePool slotted(nativeIo, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(nativeIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
						 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions())),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		const VectorManifest oldManifest = manifests.read(current, 17U).manifest();
		EXPECT_EQ(oldManifest.codebook_id, 0U);
		EXPECT_FALSE(oldManifest.pq_mapping_root_page_id.isValid());
		EXPECT_FALSE(oldManifest.adjacency_mapping_root_page_id.isValid());
		EXPECT_FALSE(metadata->findIndexRootOptional(current, PQ_MAPPING_ID).has_value());
		EXPECT_FALSE(metadata->findIndexRootOptional(current, ADJACENCY_MAPPING_ID).has_value());
		EXPECT_FALSE(metadata->findExtentLocationOptional(
					 current, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *reopened);
	VectorAdjacencyPageStore adjacencies(io, *reopened);
	VectorManifestStore manifests(io, *reopened);
	SlottedPagePool slotted(io, *reopened);
	BTreePageStore trees(slotted, *reopened);
	PhysicalSegmentAllocator segments(io, *reopened);
	VectorCodebookStore codebooks(segments, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(manifests.read(recovered, 17U).manifest().codebook_id, 0U);
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, ADJACENCY_MAPPING_ID).has_value());
	EXPECT_FALSE(reopened->findExtentLocationOptional(
				 recovered, VectorCodebookExtentIdCodec::manifestExtentId(71U)).has_value());
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *reopened, 17U, trainingRecords(), rebuildOptions());
	EXPECT_FALSE(retry.already_published);
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_EQ(reopened->findIndexRoot(published, PQ_MAPPING_ID).root_page_id, retry.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(reopened->findIndexRoot(published, ADJACENCY_MAPPING_ID).root_page_id,
			  retry.manifest.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
	   NativePostPublicationSyncFailureReportsUncertainDurabilityAndReopensCompleteGeneration) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorPageStore vectors(nativeIo, *metadata);
		VectorAdjacencyPageStore adjacencies(nativeIo, *metadata);
		VectorManifestStore manifests(nativeIo, *metadata);
		SlottedPagePool slotted(nativeIo, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(nativeIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);

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
			EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
						 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions())),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		const VectorManifest published = manifests.read(current, 17U).manifest();
		EXPECT_EQ(published.codebook_id, 71U);
		EXPECT_TRUE(published.pq_mapping_root_page_id.isValid());
		EXPECT_TRUE(published.adjacency_mapping_root_page_id.isValid());
		EXPECT_EQ(metadata->findIndexRoot(current, PQ_MAPPING_ID).root_page_id, published.pq_mapping_root_page_id);
		EXPECT_EQ(metadata->findIndexRoot(current, ADJACENCY_MAPPING_ID).root_page_id,
				  published.adjacency_mapping_root_page_id);
		current.release();
		const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
		EXPECT_TRUE(retry.already_published);
		EXPECT_EQ(retry.manifest, published);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	VectorManifestStore manifests(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	const VectorManifest published = manifests.read(recovered, 17U).manifest();
	EXPECT_EQ(published.codebook_id, 71U);
	EXPECT_TRUE(published.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(published.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(reopened->findIndexRoot(recovered, PQ_MAPPING_ID).root_page_id, published.pq_mapping_root_page_id);
	EXPECT_EQ(reopened->findIndexRoot(recovered, ADJACENCY_MAPPING_ID).root_page_id,
			  published.adjacency_mapping_root_page_id);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
	   NativePrePublicationSyncFailureRetainsCommittedDeltaOverlayAndCanRetryRetirement) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	VectorManifest deltaManifest;
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorPageStore vectors(nativeIo, *metadata);
		VectorAdjacencyPageStore adjacencies(nativeIo, *metadata);
		VectorManifestStore manifests(nativeIo, *metadata);
		SlottedPagePool slotted(nativeIo, *metadata);
		BTreePageStore trees(slotted, *metadata);
		PhysicalSegmentAllocator segments(nativeIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		materializeRawAndManifest(*metadata, vectors, manifests);
		const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
				trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
		const VectorDeltaMappingBuildOptions deltaOptions{
				{PhysicalSlotId(71U), PhysicalSlotId(75U), deltaMappingId, 16U, 4U}, 16U};
		const std::array<VectorDeltaEntry, 1U> deltaEntries{{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
		static_cast<void>(VectorDeltaMapping::build(trees, *metadata, deltaMappingId, deltaEntries, deltaOptions,
				LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U)));
		MetadataSnapshotPin deltaSnapshot = metadata->openSnapshot();
		const IndexRootLocation deltaRoot = metadata->findIndexRoot(deltaSnapshot, deltaMappingId);
		deltaSnapshot.release();
		deltaManifest = first.manifest;
		deltaManifest.delta_mapping_index_id = deltaMappingId;
		deltaManifest.delta_mapping_root_page_id = deltaRoot.root_page_id;
		deltaManifest.delta_mapping_root_generation = deltaRoot.root_generation;
		deltaManifest.delta_entry_count = 1U;
		deltaManifest.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
		overwriteManifest(nativeIo, *metadata, deltaManifest);

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
						 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
						 rebuildOptions(71U, Generation(2U)))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(manifests.read(current, 17U).manifest(), deltaManifest);
		EXPECT_EQ(metadata->findIndexRoot(current, deltaMappingId).root_page_id, deltaManifest.delta_mapping_root_page_id);
		EXPECT_EQ(VectorDeltaMapping::findOperation(current, trees, *metadata, deltaMappingId, 1U),
				  VectorDeltaOperation::VDO_INSERT);
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *reopened);
	VectorAdjacencyPageStore adjacencies(io, *reopened);
	VectorManifestStore manifests(io, *reopened);
	SlottedPagePool slotted(io, *reopened);
	BTreePageStore trees(slotted, *reopened);
	PhysicalSegmentAllocator segments(io, *reopened);
	VectorCodebookStore codebooks(segments, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(manifests.read(recovered, 17U).manifest(), deltaManifest);
	EXPECT_EQ(reopened->findIndexRoot(recovered, deltaMappingId).root_page_id,
			  deltaManifest.delta_mapping_root_page_id);
	EXPECT_EQ(VectorDeltaMapping::findOperation(recovered, trees, *reopened, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);
	recovered.release();
	const OfflineVectorBaseRebuildResult retry = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *reopened, 17U, trainingRecords(),
			rebuildOptions(71U, Generation(2U)));
	EXPECT_FALSE(retry.already_published);
	EXPECT_EQ(retry.manifest.delta_mapping_index_id, 0U);
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(manifests.read(published, 17U).manifest(), retry.manifest);
	EXPECT_FALSE(reopened->findIndexRootOptional(published, deltaMappingId).has_value());
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAnExistingCodebookWithDifferentCanonicalPayload) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorBaseRebuildOptions original = rebuildOptions();
	static_cast<void>(OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), original));

	OfflineVectorBaseRebuildOptions incompatible = original;
	// The schema is unchanged, but the bounded training sample changes the
	// deterministic payload that the durable Codebook ID claims to identify.
	incompatible.pq.training.maximum_samples = 4U;
	ASSERT_NE(DeterministicProductQuantizer::train(trainingRecords(), original.pq.training).codebookPayload(),
			  DeterministicProductQuantizer::train(trainingRecords(), incompatible.pq.training).codebookPayload());

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), incompatible)),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorManifest visible = manifests.read(snapshot, 17U).manifest();
	EXPECT_EQ(visible.codebook_id, original.pq.codebook.codebook_id);
	EXPECT_EQ(visible.adjacency_generation, original.adjacency.adjacency_generation);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, FoldsDeltaAndRetiresOldDerivedGenerationAfterPinnedSnapshotReleases) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	const uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A1);
	const VectorDeltaMappingBuildOptions deltaOptions{
			{PhysicalSlotId(71U), PhysicalSlotId(75U), deltaMappingId, 16U, 4U}, 16U};
	const std::array<VectorDeltaEntry, 1U> deltaEntries{{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, deltaMappingId, deltaEntries, deltaOptions,
			LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U)));
	MetadataSnapshotPin deltaSnapshot = metadata->openSnapshot();
	const IndexRootLocation deltaRoot = metadata->findIndexRoot(deltaSnapshot, deltaMappingId);
	deltaSnapshot.release();
	VectorManifest deltaManifest = first.manifest;
	deltaManifest.delta_mapping_index_id = deltaMappingId;
	deltaManifest.delta_mapping_root_page_id = deltaRoot.root_page_id;
	deltaManifest.delta_mapping_root_generation = deltaRoot.root_generation;
	deltaManifest.delta_entry_count = 1U;
	deltaManifest.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
	overwriteManifest(io, *metadata, deltaManifest);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const PageId oldPqPage = first.manifest.entries.front().page_id;
	const PageLocation oldPqLocation = metadata->findPageLocation(pinned, oldPqPage);
	EXPECT_EQ(manifests.read(pinned, 17U).manifest(), deltaManifest);
	EXPECT_EQ(VectorDeltaMapping::findOperation(pinned, trees, *metadata, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);

	const OfflineVectorBaseRebuildResult replacement = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
			rebuildOptions(71U, Generation(2U)));
	EXPECT_EQ(replacement.manifest.adjacency_generation, Generation(2U));
	EXPECT_EQ(replacement.manifest.delta_mapping_index_id, 0U);
	EXPECT_FALSE(replacement.manifest.delta_mapping_root_page_id.isValid());
	EXPECT_EQ(replacement.manifest.delta_entry_count, 0U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(current, 17U).manifest(), replacement.manifest);
	EXPECT_FALSE(metadata->findIndexRootOptional(current, deltaMappingId).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, oldPqPage).has_value());
	const std::optional<PageId> currentPq =
			VectorPqMapping::findPage(current, trees, *metadata, PQ_MAPPING_ID, 1U);
	ASSERT_TRUE(currentPq.has_value());
	EXPECT_NE(*currentPq, oldPqPage);
	current.release();

	EXPECT_EQ(manifests.read(pinned, 17U).manifest(), deltaManifest);
	EXPECT_TRUE(vectors.read(pinned, oldPqPage).findVectorId(1U).has_value());
	EXPECT_EQ(VectorDeltaMapping::findOperation(pinned, trees, *metadata, deltaMappingId, 1U),
			  VectorDeltaOperation::VDO_INSERT);
	vectors.reclaimRetiredFrames(oldPqLocation.segment_id);
	pinned.release();
	vectors.reclaimRetiredFrames(oldPqLocation.segment_id);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, SupportsInnerProductAndCosineAdjacencyGraphs) {
	std::vector<CanonicalVectorRecord> canonical;
	for (const DeterministicPqTrainingRecord &record: trainingRecords()) {
		canonical.push_back({record.vector_id, record.raw_bfloat16});
	}
	for (const VectorDistanceMetric metric:
		 {VectorDistanceMetric::VDM_INNER_PRODUCT, VectorDistanceMetric::VDM_COSINE}) {
		OfflineVectorAdjacencyRebuildOptions options = rebuildOptions().adjacency;
		const OfflineVectorAdjacencyPreparedGraph graph =
				OfflineVectorAdjacencyRebuild::buildGraph(metric, 4U, canonical, options);
		ASSERT_EQ(graph.records.size(), canonical.size());
		EXPECT_EQ(graph.records.front().vector_id, canonical.front().vector_id);
		for (const VectorAdjacencyMaterializationRecord &record: graph.records) {
			EXPECT_LE(record.neighbors.size(), options.maximum_degree);
		}
	}
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsRawSourceAndTypedMappingCorruptionBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	std::vector<DeterministicPqTrainingRecord> stale = trainingRecords();
	stale.front().raw_bfloat16 = bfloat16({1.0F, 0.0F, 10.0F, 10.0F});
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, stale, rebuildOptions())),
			 FormatException);
	OfflineVectorBaseRebuildOptions colliding = rebuildOptions();
	colliding.adjacency.mapping_index_id = colliding.pq.mapping_index_id;
	colliding.adjacency.mapping.tree.pool_owner_id = colliding.pq.mapping_index_id;
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), colliding)),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorManifest unchanged = manifests.read(snapshot, 17U).manifest();
	EXPECT_EQ(unchanged.codebook_id, 0U);
	EXPECT_FALSE(unchanged.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(unchanged.adjacency_mapping_root_page_id.isValid());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsEveryUnboundedOrInconsistentRebuildPlanBeforeReadingStorage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const auto reject = [&](uint64_t indexId, std::span<const DeterministicPqTrainingRecord> records,
						 const OfflineVectorBaseRebuildOptions &options) {
		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
					 trees, vectors, adjacencies, codebooks, manifests, *metadata, indexId, records, options)),
			 FormatException);
	};
	const OfflineVectorBaseRebuildOptions valid = rebuildOptions();
	const std::vector<DeterministicPqTrainingRecord> records = trainingRecords();
	reject(0U, records, valid);
	reject(17U, {}, valid);

	OfflineVectorBaseRebuildOptions invalid = valid;
	invalid.maximum_delta_mapping_pages = 0U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.maximum_delta_mapping_pages = std::numeric_limits<size_t>::max();
	reject(17U, records, invalid);
	invalid = valid;
	invalid.maximum_delta_overlay_entries = 0U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.maximum_delta_overlay_entries = std::numeric_limits<size_t>::max();
	reject(17U, records, invalid);
	invalid = valid;
	invalid.maximum_delta_overlay_entries =
			VectorDeltaMapping::maximumEntriesForPageBudget(invalid.maximum_delta_mapping_pages) + 1U;
	reject(17U, records, invalid);

	invalid = valid;
	invalid.pq.codebook.manifest_segment_id = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.codebook.generation = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.codebook.element_type = VectorCodebookElementType::VCET_UINT8;
	reject(17U, records, invalid);
	invalid = valid;
	++invalid.pq.codebook.dimensions;
	reject(17U, records, invalid);
	invalid = valid;
	++invalid.pq.codebook.subspace_count;
	reject(17U, records, invalid);
	invalid = valid;
	++invalid.pq.codebook.centroid_count;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.codebook_first_slot = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.codebook_last_slot = PhysicalSlotId(49U);
	reject(17U, records, invalid);

	invalid = valid;
	invalid.pq.pages.codebook_generation = Generation(2U);
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.pages.maximum_records = records.size() - 1U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.pages.maximum_pages = 0U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.mapping.maximum_entries = records.size() - 1U;
	reject(17U, records, invalid);
	invalid = valid;
	++invalid.pq.mapping.tree.maximum_entries;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.mapping.tree.pool_owner_id = invalid.pq.mapping_index_id + 1U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.mapping.tree.first_slot = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.mapping.tree.last_slot = PhysicalSlotId(55U);
	reject(17U, records, invalid);
	invalid = valid;
	invalid.pq.mapping.tree.maximum_pages = 0U;
	reject(17U, records, invalid);

	invalid = valid;
	invalid.adjacency.pages.maximum_records = records.size() - 1U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.pages.maximum_pages = 0U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.pages.first_slot = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.pages.last_slot = PhysicalSlotId(60U);
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.mapping.maximum_entries = records.size() - 1U;
	reject(17U, records, invalid);
	invalid = valid;
	++invalid.adjacency.mapping.tree.maximum_entries;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.mapping.tree.pool_owner_id = invalid.adjacency.mapping_index_id + 1U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.mapping.tree.first_slot = {};
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.mapping.tree.last_slot = PhysicalSlotId(65U);
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.mapping.tree.maximum_pages = 0U;
	reject(17U, records, invalid);
	invalid = valid;
	invalid.adjacency.adjacency_generation = {};
	reject(17U, records, invalid);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsCheckpointLsnExhaustionBeforePublishingACompoundBaseGeneration) {
	Superblock nearExhaustion = initialSuperblock();
	nearExhaustion.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 2U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, nearExhaustion);
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);

	std::vector<RawVectorMaterializationRecord> raw;
	for (const DeterministicPqTrainingRecord &record: trainingRecords()) {
		raw.push_back(rawRecord(record));
	}
	static_cast<void>(vectors.materializeRaw(17U, raw, {PhysicalSlotId(40U), PhysicalSlotId(40U), 16U, 4U},
																			 LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 1U)));
	VectorManifest manifest;
	manifest.index_id = 17U;
	manifest.dimensions = 4U;
	static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45U), PhysicalSlotId(45U),
																				 LogSequenceNumber(std::numeric_limits<uint64_t>::max())));

	MetadataSnapshotPin beforeRejected = metadata->openSnapshot();
	const Generation generation = beforeRejected.superblock().generation;
	beforeRejected.release();
	try {
		static_cast<void>(OfflineVectorBaseRebuild::run(trees, vectors, adjacencies, codebooks, manifests, *metadata,
																						 17U, trainingRecords(), rebuildOptions()));
		FAIL() << "Expected checkpoint-LSN exhaustion";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_ARITHMETIC_OVERFLOW);
	}
	MetadataSnapshotPin afterRejected = metadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generation);
	afterRejected.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsMixedMetadataDomainsBeforeAnyDerivedPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const std::filesystem::path foreignPath = path.string() + ".foreign";
	{
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		SlottedPagePool foreignSlotted(foreignIo, *foreignMetadata);
		BTreePageStore foreignTrees(foreignSlotted, *foreignMetadata);
		EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
						 foreignTrees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
						 rebuildOptions())),
				 FormatException);
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsAStaleSourceManifestBeforeAllocatingDerivedPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	VectorManifest stale = manifests.read(snapshot, 17U).manifest();
	snapshot.release();
	stale.dimensions = 5U;
	overwriteManifest(io, *metadata, stale);
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions())),
			 FormatException);
	snapshot = metadata->openSnapshot();
	const VectorManifest unchanged = manifests.read(snapshot, 17U).manifest();
	EXPECT_FALSE(unchanged.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(unchanged.adjacency_mapping_root_page_id.isValid());
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsDeltaCountMismatchWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	const uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A2);
	const VectorDeltaMappingBuildOptions deltaOptions{
			{PhysicalSlotId(71U), PhysicalSlotId(75U), deltaMappingId, 16U, 4U}, 16U};
	const std::array<VectorDeltaEntry, 1U> deltaEntries{{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, deltaMappingId, deltaEntries, deltaOptions,
			LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U)));
	MetadataSnapshotPin deltaSnapshot = metadata->openSnapshot();
	const IndexRootLocation deltaRoot = metadata->findIndexRoot(deltaSnapshot, deltaMappingId);
	deltaSnapshot.release();
	VectorManifest corrupt = first.manifest;
	corrupt.delta_mapping_index_id = deltaMappingId;
	corrupt.delta_mapping_root_page_id = deltaRoot.root_page_id;
	corrupt.delta_mapping_root_generation = deltaRoot.root_generation;
	corrupt.delta_entry_count = 2U;
	corrupt.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, RejectsMissingDeltaRootWithoutDiscardingVisibleOverlay) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	corrupt.delta_mapping_index_id = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A3);
	corrupt.delta_mapping_root_page_id = PageId(1U);
	corrupt.delta_mapping_root_generation = Generation(1U);
	corrupt.delta_entry_count = 1U;
	corrupt.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsADeltaRootGenerationMismatchWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	const uint64_t deltaMappingId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1A4);
	const VectorDeltaMappingBuildOptions deltaOptions{
			{PhysicalSlotId(71U), PhysicalSlotId(75U), deltaMappingId, 16U, 4U}, 16U};
	const std::array<VectorDeltaEntry, 1U> deltaEntries{{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, deltaMappingId, deltaEntries, deltaOptions,
			LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U)));
	MetadataSnapshotPin deltaSnapshot = metadata->openSnapshot();
	const IndexRootLocation deltaRoot = metadata->findIndexRoot(deltaSnapshot, deltaMappingId);
	deltaSnapshot.release();

	VectorManifest corrupt = first.manifest;
	corrupt.delta_mapping_index_id = deltaMappingId;
	corrupt.delta_mapping_root_page_id = deltaRoot.root_page_id;
	ASSERT_LT(deltaRoot.root_generation.value(), std::numeric_limits<uint64_t>::max());
	corrupt.delta_mapping_root_generation = Generation(deltaRoot.root_generation.value() + 1U);
	corrupt.delta_entry_count = deltaEntries.size();
	corrupt.committed_lsn = LogSequenceNumber(first.superblock.checkpoint_lsn.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RejectsPqManifestRecordCountCorruptionWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	auto pqEntry = std::find_if(corrupt.entries.begin(), corrupt.entries.end(), [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_NE(pqEntry, corrupt.entries.end());
	ASSERT_LT(pqEntry->record_count, std::numeric_limits<uint32_t>::max());
	++pqEntry->record_count;
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
		  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsPqMappingWithAZeroTargetBeforePublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
	const std::vector<BTreePageEntry> mappings =
			BTreeIndex(PQ_MAPPING_ID, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
	mappingSnapshot.release();
	ASSERT_FALSE(mappings.empty());
	overwriteBTreeLeafValue(*metadata, trees, PQ_MAPPING_ID, mappings.front().key, 0U);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsPqMappingWithANoncanonicalKeyLengthBeforePublishingAReplacement) {
	expectPqMappingKeyRejectedBeforeReplacement({std::byte{0U}}, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsPqMappingWithTheReservedZeroVectorIdBeforePublishingAReplacement) {
	expectPqMappingKeyRejectedBeforeReplacement(std::vector<std::byte>(sizeof(uint64_t), std::byte{0U}),
										 FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsAdjacencyMappingWithAZeroTargetBeforePublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
	const std::vector<BTreePageEntry> mappings =
			BTreeIndex(ADJACENCY_MAPPING_ID, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
	mappingSnapshot.release();
	ASSERT_FALSE(mappings.empty());
	overwriteBTreeLeafValue(*metadata, trees, ADJACENCY_MAPPING_ID, mappings.front().key, 0U);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsPqMappingThatTargetsAnExistingRawPageBeforePublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
	const std::vector<BTreePageEntry> mappings =
			BTreeIndex(PQ_MAPPING_ID, trees, *metadata).scan(mappingSnapshot, std::nullopt, 16U);
	const VectorLocation rawLocation = metadata->findVectorLocation(mappingSnapshot, 1U);
	mappingSnapshot.release();
	ASSERT_FALSE(mappings.empty());
	ASSERT_TRUE(rawLocation.page_id.isValid());
	overwriteBTreeLeafValue(*metadata, trees, PQ_MAPPING_ID, mappings.front().key, rawLocation.page_id.value());

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RejectsUnmappedPqManifestInventoryEntryWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	const auto lastPq = std::find_if(corrupt.entries.rbegin(), corrupt.entries.rend(),
										 [](const VectorManifestEntry &entry) {
											 return entry.storage_kind == VectorStorageKind::VSK_PQ;
										 });
	ASSERT_NE(lastPq, corrupt.entries.rend());
	ASSERT_LT(lastPq->page_id.value(), std::numeric_limits<uint64_t>::max());
	corrupt.entries.push_back(
			{PageId(lastPq->page_id.value() + 1U), VectorStorageKind::VSK_PQ, 1U});
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
		  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
		RejectsPqMappingsWhoseManifestInventoryWasRemovedBeforePublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	ASSERT_FALSE(corrupt.entries.empty());
	std::erase_if(corrupt.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_TRUE(corrupt.entries.empty());
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_page_id, first.manifest.pq_mapping_root_page_id);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_page_id,
			  first.manifest.adjacency_mapping_root_page_id);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RejectsPqRootGenerationMismatchWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	ASSERT_LT(corrupt.pq_mapping_root_generation.value(), std::numeric_limits<uint64_t>::max());
	corrupt.pq_mapping_root_generation = Generation(corrupt.pq_mapping_root_generation.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, PQ_MAPPING_ID).root_generation,
		  first.manifest.pq_mapping_root_generation);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RejectsAdjacencyRootGenerationMismatchWithoutPublishingAReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	VectorManifest corrupt = first.manifest;
	ASSERT_LT(corrupt.adjacency_mapping_root_generation.value(), std::numeric_limits<uint64_t>::max());
	corrupt.adjacency_mapping_root_generation =
			Generation(corrupt.adjacency_mapping_root_generation.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(71U, Generation(2U)))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, ADJACENCY_MAPPING_ID).root_generation,
		  first.manifest.adjacency_mapping_root_generation);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RequiresNewAdjacencyGenerationWhenReplacingTheCodebook) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorBaseRebuildResult first = OfflineVectorBaseRebuild::run(
			trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(),
				 rebuildOptions(72U, first.manifest.adjacency_generation))),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), first.manifest);
	EXPECT_FALSE(metadata->findExtentLocationOptional(
				 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(72U))
				 .has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest,
       RejectsAValidButFullRawManifestInventoryBeforeStagingDerivedPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	MetadataSnapshotPin originalSnapshot = metadata->openSnapshot();
	const VectorManifest original = manifests.read(originalSnapshot, 17U).manifest();
	originalSnapshot.release();
	VectorManifest corrupt = original;
	corrupt.entries.clear();
	corrupt.entries.reserve(RSS_VECTOR_MANIFEST_MAX_ENTRIES);
	for (uint64_t ordinal = 1U; ordinal <= RSS_VECTOR_MANIFEST_MAX_ENTRIES; ++ordinal) {
		corrupt.entries.push_back({PageId(UINT64_C(1000000) + ordinal), VectorStorageKind::VSK_RAW, 1U});
	}
	overwriteManifest(io, *metadata, corrupt);

	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions())),
			 FormatException);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17U).manifest(), corrupt);
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, PQ_MAPPING_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, ADJACENCY_MAPPING_ID).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, CrashBeforePublicationLeavesOnlyTheOldCompleteGenerationReachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorBaseRebuildOptions interrupted = rebuildOptions();
	interrupted.pq.manifest_publish_options.bootstrap_failpoint =
			BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), interrupted)),
			 BootstrapPublishInterrupted);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorManifestStore reopenedManifests(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const VectorManifest visible = reopenedManifests.read(snapshot, 17U).manifest();
	EXPECT_EQ(visible.codebook_id, 0U);
	EXPECT_FALSE(visible.pq_mapping_root_page_id.isValid());
	EXPECT_FALSE(visible.adjacency_mapping_root_page_id.isValid());
	EXPECT_EQ(visible.delta_entry_count, 0U);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorBaseRebuildTest, CrashAfterPublicationMakesOnlyTheCompleteNewGenerationReachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorBaseRebuildOptions interrupted = rebuildOptions();
	interrupted.pq.manifest_publish_options.bootstrap_failpoint =
			BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorBaseRebuild::run(
				 trees, vectors, adjacencies, codebooks, manifests, *metadata, 17U, trainingRecords(), interrupted)),
			 BootstrapPublishInterrupted);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorManifestStore reopenedManifests(io, *reopened);
	SlottedPagePool reopenedSlotted(io, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const VectorManifest visible = reopenedManifests.read(snapshot, 17U).manifest();
	EXPECT_EQ(visible.codebook_id, interrupted.pq.codebook.codebook_id);
	EXPECT_TRUE(visible.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(visible.adjacency_mapping_root_page_id.isValid());
	EXPECT_FALSE(visible.delta_mapping_root_page_id.isValid());
	EXPECT_EQ(visible.delta_entry_count, 0U);
	EXPECT_TRUE(VectorPqMapping::findPage(snapshot, reopenedTrees, *reopened, visible.pq_mapping_index_id, 1U)
					.has_value());
	EXPECT_TRUE(VectorAdjacencyMapping::findPage(snapshot, reopenedTrees, *reopened,
													 visible.adjacency_mapping_index_id, 1U)
					.has_value());
	snapshot.release();
}
