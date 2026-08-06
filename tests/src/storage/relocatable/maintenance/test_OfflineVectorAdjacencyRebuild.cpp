/**
 * @file test_OfflineVectorAdjacencyRebuild.cpp
 * @brief Offline derived adjacency generation publication tests.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x81);

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> bytes(values.size() * sizeof(uint16_t));
		size_t ordinal = 0U;
		for (const float value: values) {
			LittleEndianCodec::writeU16(bytes, ordinal * sizeof(uint16_t),
										static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			++ordinal;
		}
		return bytes;
	}

	std::vector<CanonicalVectorRecord> canonicalRecords() {
		return {{1, bfloat16({0.0F, 0.0F})},
				{2, bfloat16({0.0F, 1.0F})},
				{3, bfloat16({10.0F, 0.0F})},
				{4, bfloat16({10.0F, 1.0F})},
				{5, bfloat16({5.0F, 5.0F})}};
	}

	std::vector<CanonicalVectorRecord> candidateRecords(size_t count) {
		std::vector<CanonicalVectorRecord> result;
		result.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			const float x = static_cast<float>(ordinal);
			const float y = static_cast<float>((ordinal * 17U) % 23U);
			result.push_back({ordinal + 1U, bfloat16({x, y})});
		}
		return result;
	}

	RawVectorMaterializationRecord rawRecord(const CanonicalVectorRecord &record) {
		RawVectorMaterializationRecord result;
		result.vector_id = record.vector_id;
		result.bytes.resize(sizeof(uint64_t) + record.raw_bfloat16.size());
		LittleEndianCodec::writeU64(result.bytes, 0, record.vector_id);
		std::copy(record.raw_bfloat16.begin(), record.raw_bfloat16.end(), result.bytes.begin() + sizeof(uint64_t));
		return result;
	}

	VectorRawMaterializationOptions rawOptions() { return {PhysicalSlotId(40), PhysicalSlotId(40), 16, 4}; }

	OfflineVectorAdjacencyRebuildOptions options(uint64_t mappingIndexId = MAPPING_INDEX_ID,
												 Generation generation = Generation(1)) {
		OfflineVectorAdjacencyRebuildOptions result;
		result.maximum_exact_records = 16;
		result.maximum_degree = 2;
		result.adjacency_generation = generation;
		result.pages = {PhysicalSlotId(51), PhysicalSlotId(52), 16, 8};
		result.mapping_index_id = mappingIndexId;
		result.mapping.tree = {PhysicalSlotId(53), PhysicalSlotId(54), mappingIndexId, 16, 4};
		result.mapping.maximum_entries = 16;
		return result;
	}

	OfflineVectorAdjacencyRebuildOptions candidateOptions(uint64_t mappingIndexId = MAPPING_INDEX_ID,
														  Generation generation = Generation(1)) {
		OfflineVectorAdjacencyRebuildOptions result = options(mappingIndexId, generation);
		result.maximum_exact_records = 0U;
		result.build_mode = VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
		result.maximum_candidate_records = 16U;
		result.candidate_search_budget = 4U;
		result.pruning_alpha = 1.2F;
		return result;
	}

	std::vector<VectorAdjacencyRecord> expectedGraph() {
		return {{1, {2, 5}}, {2, {1, 5}}, {3, {4, 5}}, {4, {3, 5}}, {5, {2, 4}}};
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
									0,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									manifest.generation,
									manifest.committed_lsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, manifest);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, page.serialize().data(), page.serialize().size());
	}

} // namespace

class RelocatableOfflineVectorAdjacencyRebuildTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_offline_adjacency_rebuild_" + to_string(uuid));
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

	void materializeRawAndManifestRecords(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
										  VectorManifestStore &manifests,
										  std::span<const CanonicalVectorRecord> records,
										  const VectorRawMaterializationOptions &rawOptions,
										  VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2) {
		std::vector<RawVectorMaterializationRecord> raw;
		for (const CanonicalVectorRecord &record: records) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(17, raw, rawOptions, LogSequenceNumber(2)));
		VectorManifest manifest;
		manifest.index_id = 17;
		manifest.dimensions = 2;
		manifest.metric = metric;
		static_cast<void>(
				manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(3)));
	}

	void materializeRawAndManifest(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
								   VectorManifestStore &manifests,
								   VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2) {
		materializeRawAndManifestRecords(metadata, vectors, manifests, canonicalRecords(), rawOptions(), metric);
	}
};

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, PublishesOneDeterministicAdjacencyGenerationThroughTheManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorAdjacencyRebuildOptions rebuild = options();
	const OfflineVectorAdjacencyRebuildResult first = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), rebuild);
	EXPECT_FALSE(first.reused_mapping);
	EXPECT_FALSE(first.already_published);
	EXPECT_EQ(first.vector_count, 5U);
	EXPECT_EQ(first.build_work.exact_pair_distance_evaluations, 20U);
	EXPECT_EQ(first.build_work.candidate_frontier_pushes, 0U);
	EXPECT_EQ(first.build_work.candidate_prune_calls, 0U);
	EXPECT_EQ(first.manifest.adjacency_generation, rebuild.adjacency_generation);
	EXPECT_EQ(first.manifest.adjacency_mapping_index_id, rebuild.mapping_index_id);
	EXPECT_EQ(first.manifest.entry_point_vector_id, 1U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17).manifest(), first.manifest);
	const std::vector<VectorAdjacencyRecord> expected = expectedGraph();
	for (const VectorAdjacencyRecord &record: expected) {
		const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
				snapshot, trees, *metadata, rebuild.mapping_index_id, record.vector_id);
		ASSERT_TRUE(pageId.has_value());
		EXPECT_EQ(adjacencies.readRecord(snapshot, *pageId, record.vector_id), record);
	}
	snapshot.release();
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const OfflineVectorAdjacencyRebuildResult retry = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), rebuild);
	EXPECT_TRUE(retry.reused_mapping);
	EXPECT_TRUE(retry.already_published);
	EXPECT_EQ(retry.build_work, first.build_work);
	EXPECT_EQ(retry.manifest, first.manifest);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, CandidatePrunedConstructionIsBoundedAndDeterministic) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorAdjacencyRebuildOptions firstOptions = candidateOptions();
	const OfflineVectorAdjacencyRebuildResult first = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17U, canonicalRecords(), firstOptions);
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	std::vector<VectorAdjacencyRecord> firstGraph;
	for (const CanonicalVectorRecord &record: canonicalRecords()) {
		const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
				firstSnapshot, trees, *metadata, firstOptions.mapping_index_id, record.vector_id);
		ASSERT_TRUE(pageId.has_value());
		const VectorAdjacencyRecord adjacency = adjacencies.readRecord(firstSnapshot, *pageId, record.vector_id);
		EXPECT_LE(adjacency.neighbors.size(), firstOptions.maximum_degree);
		EXPECT_TRUE(std::ranges::is_sorted(adjacency.neighbors));
		EXPECT_FALSE(std::ranges::binary_search(adjacency.neighbors, record.vector_id));
		firstGraph.push_back(adjacency);
	}
	firstSnapshot.release();

	const uint64_t secondMapping = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x83);
	OfflineVectorAdjacencyRebuildOptions secondOptions = candidateOptions(secondMapping, Generation(2U));
	secondOptions.pages.first_slot = PhysicalSlotId(55U);
	secondOptions.pages.last_slot = PhysicalSlotId(55U);
	secondOptions.mapping.tree.first_slot = PhysicalSlotId(56U);
	secondOptions.mapping.tree.last_slot = PhysicalSlotId(56U);
	const OfflineVectorAdjacencyRebuildResult second = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17U, canonicalRecords(), secondOptions);
	MetadataSnapshotPin secondSnapshot = metadata->openSnapshot();
	for (size_t ordinal = 0U; ordinal < firstGraph.size(); ++ordinal) {
		const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
				secondSnapshot, trees, *metadata, second.manifest.adjacency_mapping_index_id,
				firstGraph[ordinal].vector_id);
		ASSERT_TRUE(pageId.has_value());
		EXPECT_EQ(adjacencies.readRecord(secondSnapshot, *pageId, firstGraph[ordinal].vector_id), firstGraph[ordinal]);
	}
	secondSnapshot.release();
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, UsesMetricCorrectNeighborOrderForUnequalNormVectors) {
	const std::array<CanonicalVectorRecord, 3U> records = {
			CanonicalVectorRecord{1U, bfloat16({1.0F, 0.0F})},
			CanonicalVectorRecord{2U, bfloat16({2.0F, 0.0F})},
			CanonicalVectorRecord{3U, bfloat16({10.0F, 1.0F})},
	};
	OfflineVectorAdjacencyRebuildOptions rebuild = options();
	rebuild.maximum_degree = 1U;

	const OfflineVectorAdjacencyPreparedGraph cosine = OfflineVectorAdjacencyRebuild::buildGraph(
			VectorDistanceMetric::VDM_COSINE, 2U, records, rebuild);
	const OfflineVectorAdjacencyPreparedGraph innerProduct = OfflineVectorAdjacencyRebuild::buildGraph(
			VectorDistanceMetric::VDM_INNER_PRODUCT, 2U, records, rebuild);
	ASSERT_EQ(cosine.records.size(), records.size());
	ASSERT_EQ(innerProduct.records.size(), records.size());
	EXPECT_EQ(cosine.records.front().neighbors, (std::vector<uint64_t>{2U}));
	EXPECT_EQ(innerProduct.records.front().neighbors, (std::vector<uint64_t>{3U}));
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, AppliesCandidatePruningToCosineDistances) {
	const std::array<CanonicalVectorRecord, 3U> records = {
			CanonicalVectorRecord{1U, bfloat16({1.0F, 0.0F})},
			CanonicalVectorRecord{2U, bfloat16({0.9848077F, 0.1736482F})},
			CanonicalVectorRecord{3U, bfloat16({0.9396926F, 0.3420202F})},
	};
	OfflineVectorAdjacencyRebuildOptions rebuild = candidateOptions();
	rebuild.maximum_candidate_records = records.size();
	rebuild.candidate_search_budget = records.size() - 1U;
	rebuild.maximum_degree = 2U;

	const OfflineVectorAdjacencyPreparedGraph graph = OfflineVectorAdjacencyRebuild::buildGraph(
			VectorDistanceMetric::VDM_COSINE, 2U, records, rebuild);
	ASSERT_EQ(graph.records.size(), records.size());
	EXPECT_EQ(graph.records.back().neighbors, (std::vector<uint64_t>{2U}));
	EXPECT_GT(graph.build_work.candidate_prune_pair_checks, 0U);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsZeroNormCosineRecordsBeforeAnyStaging) {
	const std::array<CanonicalVectorRecord, 2U> records = {
			CanonicalVectorRecord{1U, bfloat16({0.0F, 0.0F})},
			CanonicalVectorRecord{2U, bfloat16({1.0F, 0.0F})},
	};
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::buildGraph(
						VectorDistanceMetric::VDM_COSINE, 2U, records, options()));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, CandidatePrunedConstructionSupportsInputsBeyondTheExactLimit) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::vector<CanonicalVectorRecord> records = candidateRecords(65U);
	materializeRawAndManifestRecords(
			*metadata, vectors, manifests, records,
			VectorRawMaterializationOptions{PhysicalSlotId(40U), PhysicalSlotId(40U), 128U, 8U});
	OfflineVectorAdjacencyRebuildOptions rebuild = candidateOptions();
	rebuild.maximum_candidate_records = 128U;
	rebuild.candidate_search_budget = 16U;
	rebuild.maximum_degree = 4U;
	rebuild.pages.maximum_records = 128U;
	rebuild.mapping.maximum_entries = 128U;
	rebuild.mapping.tree.maximum_entries = 128U;

	const OfflineVectorAdjacencyRebuildResult result = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17U, records, rebuild);
	EXPECT_EQ(result.vector_count, records.size());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const CanonicalVectorRecord &record: records) {
		const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
				snapshot, trees, *metadata, rebuild.mapping_index_id, record.vector_id);
		ASSERT_TRUE(pageId.has_value());
		const VectorAdjacencyRecord adjacency = adjacencies.readRecord(snapshot, *pageId, record.vector_id);
		EXPECT_LE(adjacency.neighbors.size(), rebuild.maximum_degree);
		EXPECT_TRUE(std::ranges::is_sorted(adjacency.neighbors));
		EXPECT_FALSE(std::ranges::binary_search(adjacency.neighbors, record.vector_id));
	}
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, CandidatePrunedWorkRemainsBoundedForALargeInput) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::vector<CanonicalVectorRecord> records = candidateRecords(513U);
	materializeRawAndManifestRecords(
			*metadata, vectors, manifests, records,
			VectorRawMaterializationOptions{PhysicalSlotId(40U), PhysicalSlotId(40U), records.size(), 32U});
	OfflineVectorAdjacencyRebuildOptions rebuild = candidateOptions();
	rebuild.maximum_candidate_records = records.size();
	rebuild.candidate_search_budget = 32U;
	rebuild.maximum_degree = 8U;
	rebuild.pages.maximum_records = records.size();
	rebuild.pages.maximum_pages = 64U;
	rebuild.mapping.maximum_entries = records.size();
	rebuild.mapping.tree.maximum_entries = records.size();
	rebuild.mapping.tree.maximum_pages = 64U;

	const OfflineVectorAdjacencyRebuildResult result = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17U, records, rebuild);
	const uint64_t recordCount = static_cast<uint64_t>(records.size());
	const uint64_t degree = static_cast<uint64_t>(rebuild.maximum_degree);
	const uint64_t searchBudget = static_cast<uint64_t>(rebuild.candidate_search_budget);
	EXPECT_EQ(result.vector_count, recordCount);
	EXPECT_EQ(result.build_work.exact_pair_distance_evaluations, 0U);
	EXPECT_LE(result.build_work.candidate_frontier_pops, recordCount * searchBudget);
	EXPECT_GE(result.build_work.candidate_frontier_pushes, result.build_work.candidate_frontier_pops);
	EXPECT_LE(result.build_work.candidate_frontier_peak, recordCount);
	EXPECT_EQ(result.build_work.candidate_prune_calls,
			  recordCount + result.build_work.candidate_reverse_link_updates);
	EXPECT_LE(result.build_work.candidate_reverse_link_updates, recordCount * degree);
	EXPECT_LE(result.build_work.candidate_prune_candidates,
			  recordCount * searchBudget + result.build_work.candidate_reverse_link_updates * (degree + 1U));
	EXPECT_LE(result.build_work.candidate_prune_pair_checks,
			  result.build_work.candidate_prune_candidates * degree);

	// These bounds prove a candidate-pruned rebuild did not silently fall back
	// to an all-pairs traversal for the large-input path.
	EXPECT_LT(result.build_work.candidate_frontier_pops, recordCount * (recordCount - 1U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const size_t ordinal: {size_t{0U}, records.size() / 2U, records.size() - 1U}) {
		const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
				snapshot, trees, *metadata, rebuild.mapping_index_id, records[ordinal].vector_id);
		ASSERT_TRUE(pageId.has_value());
		const VectorAdjacencyRecord adjacency = adjacencies.readRecord(snapshot, *pageId, records[ordinal].vector_id);
		EXPECT_LE(adjacency.neighbors.size(), rebuild.maximum_degree);
		EXPECT_TRUE(std::ranges::is_sorted(adjacency.neighbors));
		EXPECT_FALSE(std::ranges::binary_search(adjacency.neighbors, records[ordinal].vector_id));
	}
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RecoversAStagedMappingAndRetriesOnlyTheManifestSwitch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	OfflineVectorAdjacencyRebuildOptions interrupted = options();
	interrupted.manifest_publish_options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	  17, canonicalRecords(), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopened);
	VectorAdjacencyPageStore reopenedAdjacencies(io, *reopened);
	VectorManifestStore reopenedManifests(io, *reopened);
	SlottedPagePool reopenedSlotted(io, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	const OfflineVectorAdjacencyRebuildResult resumed =
			OfflineVectorAdjacencyRebuild::run(reopenedTrees, reopenedVectors, reopenedAdjacencies, reopenedManifests,
											   *reopened, 17, canonicalRecords(), options());
	EXPECT_TRUE(resumed.reused_mapping);
	EXPECT_FALSE(resumed.already_published);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedManifests.read(snapshot, 17).manifest(), resumed.manifest);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, KeepsAnOlderManifestAndMappingReadableThroughAPinnedSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorAdjacencyRebuildResult first = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), options());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const VectorManifest oldManifest = manifests.read(oldSnapshot, 17).manifest();
	const uint64_t secondMapping = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x82);
	OfflineVectorAdjacencyRebuildOptions next = options(secondMapping, Generation(2));
	next.pages.first_slot = PhysicalSlotId(55);
	next.pages.last_slot = PhysicalSlotId(55);
	next.mapping.tree.first_slot = PhysicalSlotId(56);
	next.mapping.tree.last_slot = PhysicalSlotId(56);
	const OfflineVectorAdjacencyRebuildResult second = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), next);
	EXPECT_EQ(manifests.read(oldSnapshot, 17).manifest(), oldManifest);
	const std::optional<PageId> oldPage = VectorAdjacencyMapping::findPage(
			oldSnapshot, trees, *metadata, first.manifest.adjacency_mapping_index_id, 1);
	ASSERT_TRUE(oldPage.has_value());
	EXPECT_EQ(adjacencies.readRecord(oldSnapshot, *oldPage, 1), (VectorAdjacencyRecord{1, {2, 5}}));
	oldSnapshot.release();
	EXPECT_EQ(second.manifest.adjacency_generation, Generation(2));
	EXPECT_EQ(second.manifest.adjacency_mapping_index_id, secondMapping);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAnUnknownMetricBeforeGraphConstruction) {
	const std::vector<CanonicalVectorRecord> records = canonicalRecords();
	OfflineVectorAdjacencyRebuildOptions rebuild = options();
	rebuild.maximum_exact_records = records.size();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::buildGraph(
						static_cast<VectorDistanceMetric>(std::numeric_limits<uint16_t>::max()), 2U, records, rebuild));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsRawInputThatDoesNotMatchTheDurableVectorLocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	std::vector<CanonicalVectorRecord> mismatched = canonicalRecords();
	mismatched.front().raw_bfloat16 = bfloat16({1.0F, 1.0F});
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, mismatched, options()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsEveryUnboundedOrIncompatibleRebuildOptionBeforeAccessingData) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const auto expectInvalid = [&](const std::function<void(OfflineVectorAdjacencyRebuildOptions &)> &mutate) {
		OfflineVectorAdjacencyRebuildOptions invalid = options();
		mutate(invalid);
		expectFormatError(
				[&] {
					static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests,
																		 *metadata, 17, canonicalRecords(), invalid));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	expectInvalid([](auto &value) { value.maximum_exact_records = 0; });
	expectInvalid([](auto &value) { value.maximum_exact_records = canonicalRecords().size() - 1U; });
	expectInvalid([](auto &value) {
		value.build_mode = static_cast<VectorAdjacencyBuildMode>(std::numeric_limits<uint8_t>::max());
	});
	expectInvalid([](auto &value) { value.maximum_degree = 0; });
	expectInvalid([](auto &value) { value.maximum_degree = RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS + 1U; });
	expectInvalid([](auto &value) { value.adjacency_generation = Generation{}; });
	expectInvalid([](auto &value) { value.pages.maximum_records = 0; });
	expectInvalid([](auto &value) { value.pages.maximum_records = canonicalRecords().size() - 1U; });
	expectInvalid([](auto &value) { value.pages.maximum_pages = 0; });
	expectInvalid([](auto &value) { value.pages.first_slot = PhysicalSlotId{}; });
	expectInvalid([](auto &value) { value.pages.first_slot = PhysicalSlotId(53); });
	expectInvalid([](auto &value) { value.mapping.maximum_entries = 0; });
	expectInvalid([](auto &value) { value.mapping.maximum_entries = canonicalRecords().size() - 1U; });
	expectInvalid([](auto &value) { value.mapping.tree.maximum_entries = 15; });
	expectInvalid([](auto &value) { value.mapping.tree.pool_owner_id = MAPPING_INDEX_ID + 1U; });
	expectInvalid([](auto &value) { value.mapping.tree.last_slot = PhysicalSlotId{}; });
	expectInvalid([](auto &value) { value.mapping.tree.first_slot = PhysicalSlotId(55); });
	const auto expectInvalidCandidate = [&](const std::function<void(OfflineVectorAdjacencyRebuildOptions &)> &mutate) {
		OfflineVectorAdjacencyRebuildOptions invalid = candidateOptions();
		mutate(invalid);
		expectFormatError(
				[&] {
					static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests,
																		 *metadata, 17, canonicalRecords(), invalid));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	expectInvalidCandidate([](auto &value) { value.maximum_candidate_records = 0U; });
	expectInvalidCandidate([](auto &value) { value.maximum_candidate_records = canonicalRecords().size() - 1U; });
	expectInvalidCandidate([](auto &value) { value.candidate_search_budget = 0U; });
	expectInvalidCandidate([](auto &value) { value.candidate_search_budget = 17U; });
	expectInvalidCandidate([](auto &value) { value.pruning_alpha = 0.5F; });
	expectInvalidCandidate([](auto &value) { value.pruning_alpha = std::numeric_limits<float>::quiet_NaN(); });
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 0, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, {}, options()));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsMalformedCanonicalCoordinatesBeforeStaging) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	std::vector<CanonicalVectorRecord> malformed = canonicalRecords();
	malformed.front().vector_id = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																			 17, malformed, options()));
			},
			FormatError::FERR_INVALID_HEADER);
	malformed = canonicalRecords();
	malformed.front().raw_bfloat16.pop_back();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, malformed, options()));
			},
			FormatError::FERR_INVALID_HEADER);
	malformed = canonicalRecords();
	std::swap(malformed[0], malformed[1]);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, malformed, options()));
			},
			FormatError::FERR_INVALID_HEADER);
	malformed = canonicalRecords();
	LittleEndianCodec::writeU16(malformed.front().raw_bfloat16, 0, 0x7F80U);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, malformed, options()));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAnExistingMappingThatNoLongerCoversTheCanonicalVectorSet) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
														 canonicalRecords(), options()));
	ASSERT_TRUE(trees.erase(MAPPING_INDEX_ID, VectorAdjacencyMapping::key(1), LogSequenceNumber(7)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsANonAdvancingGenerationForADifferentVisibleMapping) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
														 canonicalRecords(), options()));
	const uint64_t secondMapping = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x83);
	OfflineVectorAdjacencyRebuildOptions invalid = options(secondMapping, Generation(1));
	invalid.pages.first_slot = PhysicalSlotId(55);
	invalid.pages.last_slot = PhysicalSlotId(55);
	invalid.mapping.tree.first_slot = PhysicalSlotId(56);
	invalid.mapping.tree.last_slot = PhysicalSlotId(56);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, canonicalRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAnExistingMappingWithDifferentKeysOrAdjacencyContents) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
														 canonicalRecords(), options()));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> secondPage =
			VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 2);
	snapshot.release();
	ASSERT_TRUE(secondPage.has_value());
	ASSERT_TRUE(trees.erase(MAPPING_INDEX_ID, VectorAdjacencyMapping::key(1), LogSequenceNumber(7)));
	static_cast<void>(
			trees.upsert(MAPPING_INDEX_ID, VectorAdjacencyMapping::key(9), secondPage->value(), LogSequenceNumber(8)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAnExistingMappingWithANullPageReference) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
																			 canonicalRecords(), options()));
	ASSERT_TRUE(trees.erase(MAPPING_INDEX_ID, VectorAdjacencyMapping::key(1), LogSequenceNumber(7)));
	static_cast<void>(trees.upsert(MAPPING_INDEX_ID, VectorAdjacencyMapping::key(1), 0U, LogSequenceNumber(8)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																			 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAnExistingMappingWithDifferentAdjacencyContents) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
														 canonicalRecords(), options()));
	OfflineVectorAdjacencyRebuildOptions differentDegree = options(MAPPING_INDEX_ID, Generation(2));
	differentDegree.maximum_degree = 1;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, canonicalRecords(), differentDegree));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAVisibleManifestWhoseAdjacencyRootNoLongerMatchesMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorAdjacencyRebuildResult first = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), options());
	VectorManifest corrupt = first.manifest;
	corrupt.adjacency_mapping_root_page_id = PageId(first.manifest.adjacency_mapping_root_page_id.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																	 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsAVisibleManifestWithAnInconsistentEntryPoint) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorAdjacencyRebuildResult first = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), options());
	VectorManifest corrupt = first.manifest;
	corrupt.entry_point_vector_id = 2U;
	overwriteManifest(io, *metadata, corrupt);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata,
																			 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineVectorAdjacencyRebuildTest, RejectsStoresBoundToADifferentMetadataDatabase) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::filesystem::path foreignPath = path.string() + ".foreign";
	const std::filesystem::path foreignWalPath = foreignPath.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																		   std::ios::trunc);
	auto foreignWalFile = std::make_shared<std::fstream>(foreignWalPath, std::ios::in | std::ios::out |
																				 std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	ASSERT_TRUE(foreignWalFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignWalIo = std::make_shared<StorageIO>(foreignWalFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorAdjacencyRebuild::run(
						trees, vectors, adjacencies, manifests, *foreignMetadata, 17, canonicalRecords(), options()));
			},
			FormatError::FERR_INVALID_STATE);
	foreignMetadata.reset();
	foreignIo.reset();
	foreignWalIo.reset();
	foreignFile->close();
	foreignWalFile->close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}
