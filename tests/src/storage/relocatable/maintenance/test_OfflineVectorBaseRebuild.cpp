/**
 * @file test_OfflineVectorBaseRebuild.cpp
 * @brief Atomic PQ, adjacency, and delta-folding Vector base-rebuild tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorBaseRebuild.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"

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
		std::vector<RawVectorMaterializationRecord> raw;
		for (const DeterministicPqTrainingRecord &record: trainingRecords()) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(
				17U, raw, {PhysicalSlotId(40U), PhysicalSlotId(40U), 16U, 4U}, LogSequenceNumber(2U)));
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
