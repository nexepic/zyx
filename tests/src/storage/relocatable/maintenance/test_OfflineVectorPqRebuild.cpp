/**
 * @file test_OfflineVectorPqRebuild.cpp
 * @brief Offline derived PQ generation publication tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"

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

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> bytes(values.size() * sizeof(uint16_t));
		size_t index = 0;
		for (const float value: values) {
			LittleEndianCodec::writeU16(bytes, index * sizeof(uint16_t),
										static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			++index;
		}
		return bytes;
	}

	std::vector<DeterministicPqTrainingRecord> trainingRecords() {
		return {{1, bfloat16({0.0F, 0.0F, 10.0F, 10.0F})},
				{2, bfloat16({0.0F, 1.0F, 9.0F, 10.0F})},
				{3, bfloat16({9.0F, 10.0F, 0.0F, 0.0F})},
				{4, bfloat16({10.0F, 9.0F, 1.0F, 0.0F})},
				{5, bfloat16({5.0F, 5.0F, 5.0F, 5.0F})}};
	}

	std::vector<DeterministicPqTrainingRecord> trainingRecordsSpanningPqPages(size_t count, uint32_t dimensions) {
		std::vector<DeterministicPqTrainingRecord> records;
		records.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			std::vector<std::byte> raw(static_cast<size_t>(dimensions) * sizeof(uint16_t));
			for (uint32_t coordinate = 0U; coordinate < dimensions; ++coordinate) {
				const float value = static_cast<float>((ordinal + coordinate) % 23U);
				LittleEndianCodec::writeU16(raw, static_cast<size_t>(coordinate) * sizeof(uint16_t),
										 static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			}
			records.push_back({ordinal + 1U, std::move(raw)});
		}
		return records;
	}

	RawVectorMaterializationRecord rawRecord(const DeterministicPqTrainingRecord &record) {
		RawVectorMaterializationRecord result;
		result.vector_id = record.vector_id;
		result.bytes.resize(sizeof(uint64_t) + record.raw_bfloat16.size());
		LittleEndianCodec::writeU64(result.bytes, 0, record.vector_id);
		std::copy(record.raw_bfloat16.begin(), record.raw_bfloat16.end(), result.bytes.begin() + sizeof(uint64_t));
		return result;
	}

	VectorRawMaterializationOptions rawOptions() { return {PhysicalSlotId(40), PhysicalSlotId(40), 16, 4}; }

	OfflineVectorPqRebuildOptions rebuildOptions() {
		constexpr uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x99);
		OfflineVectorPqRebuildOptions result;
		result.training = {4, 2, 2, 3, 16, 5};
		result.codebook.codebook_id = 71;
		result.codebook.manifest_segment_id = SegmentId(71);
		result.codebook.generation = Generation(1);
		result.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.codebook.dimensions = result.training.dimensions;
		result.codebook.subspace_count = result.training.subspace_count;
		result.codebook.centroid_count = result.training.centroid_count;
		result.codebook_first_slot = PhysicalSlotId(50);
		result.codebook_last_slot = PhysicalSlotId(50);
		result.pages.first_slot = PhysicalSlotId(51);
		result.pages.last_slot = PhysicalSlotId(55);
		result.pages.codebook_generation = result.codebook.generation;
		result.pages.maximum_records = 16;
		result.pages.maximum_pages = 4;
		result.mapping_index_id = mappingIndexId;
		result.mapping.tree = {PhysicalSlotId(56), PhysicalSlotId(60), mappingIndexId, 16, 4};
		result.mapping.maximum_entries = 16;
		return result;
	}

	OfflineVectorPqRebuildOptions replacementRebuildOptions() {
		OfflineVectorPqRebuildOptions result = rebuildOptions();
		result.codebook.codebook_id = 81U;
		result.codebook.manifest_segment_id = SegmentId(81U);
		result.codebook_first_slot = PhysicalSlotId(61U);
		result.codebook_last_slot = PhysicalSlotId(61U);
		result.pages.first_slot = PhysicalSlotId(62U);
		result.pages.last_slot = PhysicalSlotId(65U);
		result.mapping.tree.first_slot = PhysicalSlotId(66U);
		result.mapping.tree.last_slot = PhysicalSlotId(70U);
		return result;
	}

} // namespace

class RelocatableOfflineVectorPqRebuildTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_offline_pq_rebuild_" + to_string(uuid));
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

	void materializeRawAndManifest(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
								   VectorManifestStore &manifests, bool includeRawManifestEntry = false,
								   bool includePqManifestEntry = false) {
		const std::vector<DeterministicPqTrainingRecord> training = trainingRecords();
		std::vector<RawVectorMaterializationRecord> raw;
		raw.reserve(training.size());
		for (const DeterministicPqTrainingRecord &record: training) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(17, raw, rawOptions(), LogSequenceNumber(2)));
		VectorManifest manifest;
		manifest.index_id = 17;
		manifest.dimensions = 4;
		if (includeRawManifestEntry) {
			manifest.entries = {{PageId(900), VectorStorageKind::VSK_RAW, 1}};
		}
		if (includePqManifestEntry) {
			manifest.entries.push_back({PageId(901), VectorStorageKind::VSK_PQ, 1});
		}
		static_cast<void>(
				manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(3)));
	}

	void materializeRawAndManifest(RelocatableMetadataStore &metadata, VectorPageStore &vectors,
										 VectorManifestStore &manifests,
										 std::span<const DeterministicPqTrainingRecord> training,
										 const VectorRawMaterializationOptions &options) {
		std::vector<RawVectorMaterializationRecord> raw;
		raw.reserve(training.size());
		for (const DeterministicPqTrainingRecord &record: training) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(17U, raw, options, LogSequenceNumber(2U)));
		VectorManifest manifest;
		manifest.index_id = 17U;
		manifest.dimensions = 4U;
		static_cast<void>(
				manifests.materialize(manifest, PhysicalSlotId(45U), PhysicalSlotId(45U), LogSequenceNumber(3U)));
	}

	void stageMapping(RelocatableMetadataStore &metadata, VectorPageStore &vectors, BTreePageStore &trees,
					  const OfflineVectorPqRebuildOptions &options) {
		const DeterministicProductQuantizer quantizer =
				DeterministicProductQuantizer::train(trainingRecords(), options.training);
		std::vector<PqVectorMaterializationRecord> records;
		for (const DeterministicPqTrainingRecord &record: trainingRecords()) {
			records.push_back({record.vector_id, quantizer.encode(record.raw_bfloat16)});
		}
		const VectorPqMaterializationResult pages =
				vectors.materializePq(17, records, options.pages, LogSequenceNumber(4));
		static_cast<void>(VectorPqMapping::build(trees, metadata, options.mapping_index_id, pages.mappings,
												 options.mapping, LogSequenceNumber(5)));
	}

	void overwriteManifest(RelocatableMetadataStore &metadata, const VectorManifest &manifest) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
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

	void overwriteVectorPageRecord(RelocatableMetadataStore &metadata, VectorPageStore &vectors, PageId pageId,
								   uint64_t vectorId, std::span<const std::byte> replacement) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		VectorPage page = vectors.read(snapshot, pageId);
		const std::optional<uint16_t> slot = page.findVectorId(vectorId);
		ASSERT_TRUE(slot.has_value());
		ASSERT_EQ(replacement.size(), page.header().record_bytes);
		page.overwrite(*slot, replacement);
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const std::vector<std::byte> bytes = page.serialize();
		io->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(), bytes.size());
		io->sync();
	}

	void pointManifestAtPqMapping(RelocatableMetadataStore &metadata, VectorManifestStore &manifests, uint64_t indexId,
								  const IndexRootLocation &root) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		VectorManifest candidate = manifests.read(snapshot, indexId).manifest();
		snapshot.release();
		candidate.pq_mapping_index_id = root.index_id;
		candidate.pq_mapping_root_page_id = root.root_page_id;
		candidate.pq_mapping_root_generation = root.root_generation;
		overwriteManifest(metadata, candidate);
	}
};

TEST_F(RelocatableOfflineVectorPqRebuildTest, PublishesOnlyTheCompletePqGenerationThroughTheManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	const OfflineVectorPqRebuildResult first = OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests,
																		   *metadata, 17, trainingRecords(), options);
	EXPECT_FALSE(first.reused_codebook);
	EXPECT_FALSE(first.reused_mapping);
	EXPECT_FALSE(first.already_published);
	EXPECT_EQ(first.vector_count, 5U);
	EXPECT_EQ(first.manifest.codebook_id, options.codebook.codebook_id);
	EXPECT_EQ(first.manifest.codebook_generation, options.codebook.generation);
	EXPECT_EQ(first.manifest.pq_mapping_index_id, options.mapping_index_id);
	ASSERT_EQ(first.manifest.entries.size(), 1U);
	EXPECT_EQ(first.manifest.entries.front().storage_kind, VectorStorageKind::VSK_PQ);
	EXPECT_EQ(first.manifest.entries.front().record_count, 5U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(snapshot, 17).manifest(), first.manifest);
	const std::optional<PageId> pageId =
			VectorPqMapping::findPage(snapshot, trees, *metadata, options.mapping_index_id, 3);
	ASSERT_TRUE(pageId.has_value());
	const VectorPage page = vectors.read(snapshot, *pageId);
	const std::optional<uint16_t> slot = page.findVectorId(3);
	ASSERT_TRUE(slot.has_value());
	EXPECT_EQ(LittleEndianCodec::readU64(page.read(*slot), 0), 3U);
	EXPECT_EQ(codebooks.read(snapshot, options.codebook.codebook_id).header.subspace_count, 2U);
	snapshot.release();

	const OfflineVectorPqRebuildResult retry = OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests,
																		   *metadata, 17, trainingRecords(), options);
	EXPECT_TRUE(retry.reused_codebook);
	EXPECT_TRUE(retry.reused_mapping);
	EXPECT_TRUE(retry.already_published);
	EXPECT_EQ(retry.manifest, first.manifest);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RebuildsExistingPqGenerationWithoutBreakingPinnedSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorPqRebuildResult first = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const IndexRootLocation oldRoot = metadata->findIndexRoot(pinned, first.manifest.pq_mapping_index_id);
	ASSERT_EQ(first.manifest.entries.size(), 1U);
	const PageId oldPqPageId = first.manifest.entries.front().page_id;
	const PageLocation oldPqLocation = metadata->findPageLocation(pinned, oldPqPageId);
	const PageLocation oldMappingLocation = metadata->findPageLocation(pinned, oldRoot.root_page_id);

	const OfflineVectorPqRebuildOptions replacementOptions = replacementRebuildOptions();
	const OfflineVectorPqRebuildResult replacement = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), replacementOptions);
	EXPECT_FALSE(replacement.already_published);
	EXPECT_FALSE(replacement.reused_mapping);
	EXPECT_FALSE(replacement.reused_codebook);
	EXPECT_EQ(replacement.superblock.generation, Generation(first.superblock.generation.value() + 1U));
	EXPECT_EQ(replacement.manifest.codebook_id, replacementOptions.codebook.codebook_id);
	EXPECT_NE(replacement.manifest.pq_mapping_root_page_id, oldRoot.root_page_id);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(current, 17U).manifest(), replacement.manifest);
	EXPECT_FALSE(metadata->findPageLocationOptional(current, oldPqPageId).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, oldRoot.root_page_id).has_value());
	const std::optional<PageId> currentPqPage =
			VectorPqMapping::findPage(current, trees, *metadata, replacementOptions.mapping_index_id, 3U);
	ASSERT_TRUE(currentPqPage.has_value());
	EXPECT_NE(*currentPqPage, oldPqPageId);

	EXPECT_EQ(manifests.read(pinned, 17U).manifest(), first.manifest);
	const std::optional<PageId> pinnedPqPage =
			VectorPqMapping::findPage(pinned, trees, *metadata, first.manifest.pq_mapping_index_id, 3U);
	ASSERT_TRUE(pinnedPqPage.has_value());
	EXPECT_EQ(*pinnedPqPage, oldPqPageId);
	EXPECT_TRUE(vectors.read(pinned, oldPqPageId).findVectorId(3U).has_value());

	// A current snapshot cannot read retired frames, while the pinned source can.
	expectFormatError([&] { static_cast<void>(vectors.read(current, oldPqPageId)); }, FormatError::FERR_INVALID_STATE);
	current.release();
	vectors.reclaimRetiredFrames(oldPqLocation.segment_id);
	trees.reclaimRetiredFrames(oldMappingLocation.segment_id);
	pinned.release();
	vectors.reclaimRetiredFrames(oldPqLocation.segment_id);
	trees.reclaimRetiredFrames(oldMappingLocation.segment_id);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, ReplacementRebuildRecoversAsOneCompleteGenerationAtPublishBoundaries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorPqRebuildResult first = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), rebuildOptions());
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	const IndexRootLocation oldRoot = metadata->findIndexRoot(firstSnapshot, first.manifest.pq_mapping_index_id);
	ASSERT_EQ(first.manifest.entries.size(), 1U);
	const PageId oldPqPageId = first.manifest.entries.front().page_id;
	firstSnapshot.release();

	OfflineVectorPqRebuildOptions beforeInactiveWrite = replacementRebuildOptions();
	beforeInactiveWrite.manifest_publish_options.bootstrap_failpoint =
			BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
																									 trainingRecords(), beforeInactiveWrite)),
			 BootstrapPublishInterrupted);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_EQ(manifests.read(before, 17U).manifest(), first.manifest);
	EXPECT_EQ(metadata->findIndexRoot(before, first.manifest.pq_mapping_index_id), oldRoot);
	EXPECT_TRUE(metadata->findPageLocationOptional(before, oldPqPageId).has_value());
	before.release();

	metadata.reset();
	auto afterRecord = RelocatableMetadataStore::open(io);
	VectorPageStore afterRecordVectors(io, *afterRecord);
	VectorManifestStore afterRecordManifests(io, *afterRecord);
	SlottedPagePool afterRecordSlotted(io, *afterRecord);
	BTreePageStore afterRecordTrees(afterRecordSlotted, *afterRecord);
	PhysicalSegmentAllocator afterRecordSegments(io, *afterRecord);
	VectorCodebookStore afterRecordCodebooks(afterRecordSegments, *afterRecord);
	MetadataSnapshotPin afterRecordSnapshot = afterRecord->openSnapshot();
	EXPECT_EQ(afterRecordManifests.read(afterRecordSnapshot, 17U).manifest(), first.manifest);
	EXPECT_EQ(afterRecord->findIndexRoot(afterRecordSnapshot, first.manifest.pq_mapping_index_id), oldRoot);
	afterRecordSnapshot.release();

	OfflineVectorPqRebuildOptions afterInactiveWrite = replacementRebuildOptions();
	afterInactiveWrite.manifest_publish_options.bootstrap_failpoint =
			BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorPqRebuild::run(afterRecordTrees, afterRecordVectors, afterRecordCodebooks,
																							 afterRecordManifests, *afterRecord, 17U, trainingRecords(),
																							 afterInactiveWrite)),
			 BootstrapPublishInterrupted);
	afterRecord.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	VectorPageStore recoveredVectors(io, *recovered);
	VectorManifestStore recoveredManifests(io, *recovered);
	SlottedPagePool recoveredSlotted(io, *recovered);
	BTreePageStore recoveredTrees(recoveredSlotted, *recovered);
	PhysicalSegmentAllocator recoveredSegments(io, *recovered);
	VectorCodebookStore recoveredCodebooks(recoveredSegments, *recovered);
	MetadataSnapshotPin current = recovered->openSnapshot();
	const VectorManifest recoveredManifest = recoveredManifests.read(current, 17U).manifest();
	EXPECT_EQ(recoveredManifest.codebook_id, afterInactiveWrite.codebook.codebook_id);
	EXPECT_EQ(recoveredManifest.codebook_generation, afterInactiveWrite.codebook.generation);
	EXPECT_EQ(recoveredManifest.pq_mapping_index_id, first.manifest.pq_mapping_index_id);
	EXPECT_NE(recoveredManifest.pq_mapping_root_page_id, oldRoot.root_page_id);
	EXPECT_FALSE(recovered->findPageLocationOptional(current, oldPqPageId).has_value());
	const std::optional<PageId> currentPqPage = VectorPqMapping::findPage(
			current, recoveredTrees, *recovered, recoveredManifest.pq_mapping_index_id, 3U);
	ASSERT_TRUE(currentPqPage.has_value());
	EXPECT_NE(*currentPqPage, oldPqPageId);
	EXPECT_TRUE(recoveredVectors.read(current, *currentPqPage).findVectorId(3U).has_value());
	EXPECT_EQ(recoveredCodebooks.read(current, afterInactiveWrite.codebook.codebook_id).header.generation,
		  afterInactiveWrite.codebook.generation);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, ReclaimsOnlyUnpublishedPqPagesAfterManifestPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), options);
	VectorPqMaterializationOptions stagedOptions = options.pages;
	stagedOptions.first_slot = PhysicalSlotId(61);
	stagedOptions.last_slot = PhysicalSlotId(61);
	const std::array<PqVectorMaterializationRecord, 1> stagedRecords{{{99, {0, 0}}}};
	MetadataSnapshotPin beforeStage = metadata->openSnapshot();
	const LogSequenceNumber stageLsn(beforeStage.superblock().checkpoint_lsn.value() + 1U);
	beforeStage.release();
	const VectorPqMaterializationResult staged = vectors.materializePq(17, stagedRecords, stagedOptions, stageLsn);
	ASSERT_EQ(staged.pages.size(), 1U);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const std::optional<PageId> visible =
			VectorPqMapping::findPage(pinned, trees, *metadata, published.manifest.pq_mapping_index_id, 1);
	ASSERT_TRUE(visible.has_value());
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const LogSequenceNumber reclaimLsn(stageLsn.value() + 1U);
	const DerivedObjectReachabilityReclaimReport report = reclaimer.reclaimPq({16, 16, 16, 16, 16}, reclaimLsn);
	EXPECT_EQ(report.reachable_pq_pages, 1U);
	EXPECT_EQ(report.derived_pq_pages_retired, staged.pages.size());
	EXPECT_TRUE(vectors.read(pinned, staged.pages.front().page_id).findVectorId(99).has_value());
	EXPECT_TRUE(vectors.read(pinned, *visible).findVectorId(1).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, staged.pages.front().page_id).has_value());
	EXPECT_TRUE(vectors.read(current, *visible).findVectorId(1).has_value());
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, BoundsEveryReachablePqCandidateDuringReclamation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	OfflineVectorPqRebuildOptions options = rebuildOptions();
	options.pages.maximum_pages = trainingRecords().size();
	std::vector<PqVectorMaterializationRecord> records;
	for (uint64_t vectorId = 1; vectorId <= trainingRecords().size(); ++vectorId) {
		records.push_back({vectorId, std::vector<uint8_t>(2000U, static_cast<uint8_t>(vectorId))});
	}
	const VectorPqMaterializationResult pq = vectors.materializePq(17, records, options.pages, LogSequenceNumber(4));
	ASSERT_EQ(pq.pages.size(), trainingRecords().size());
	static_cast<void>(VectorPqMapping::build(trees, *metadata, options.mapping_index_id, pq.mappings, options.mapping,
											 LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, options.mapping_index_id);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, root);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport complete =
			reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(8));
	EXPECT_EQ(complete.reachable_pq_pages, trainingRecords().size());
	EXPECT_EQ(complete.derived_pq_pages_retired, 0U);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 1, 16}, LogSequenceNumber(8))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenTheManifestRootIsNotCurrent) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	VectorManifest corrupt = published.manifest;
	corrupt.pq_mapping_root_generation = Generation(corrupt.pq_mapping_root_generation.value() + 1U);
	overwriteManifest(*metadata, corrupt);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(8))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingTargetsARawPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageId rawPageId = metadata->findVectorLocation(snapshot, 1).page_id;
	snapshot.release();
	static_cast<void>(trees.upsert(published.manifest.pq_mapping_index_id, VectorPqMapping::key(1), rawPageId.value(),
								   LogSequenceNumber(8)));
	snapshot = metadata->openSnapshot();
	const IndexRootLocation replacementRoot = metadata->findIndexRoot(snapshot, published.manifest.pq_mapping_index_id);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, replacementRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(9))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingTargetsTheWrongRecord) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	VectorPqMaterializationOptions stagedOptions = rebuildOptions().pages;
	stagedOptions.first_slot = PhysicalSlotId(61);
	stagedOptions.last_slot = PhysicalSlotId(61);
	const std::array<PqVectorMaterializationRecord, 1> unrelated{{{99, {0, 0}}}};
	const VectorPqMaterializationResult staged =
			vectors.materializePq(17, unrelated, stagedOptions, LogSequenceNumber(8));
	static_cast<void>(trees.upsert(published.manifest.pq_mapping_index_id, VectorPqMapping::key(1),
								   staged.pages.front().page_id.value(), LogSequenceNumber(9)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation replacementRoot = metadata->findIndexRoot(snapshot, published.manifest.pq_mapping_index_id);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, replacementRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(10))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenTheMappingDoesNotCoverRawVectors) {
	constexpr uint64_t incompleteMappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA2);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> pageId =
			VectorPqMapping::findPage(snapshot, trees, *metadata, published.manifest.pq_mapping_index_id, 1);
	ASSERT_TRUE(pageId.has_value());
	snapshot.release();
	VectorPqMappingBuildOptions incompleteOptions = rebuildOptions().mapping;
	incompleteOptions.tree.pool_owner_id = incompleteMappingIndexId;
	incompleteOptions.tree.first_slot = PhysicalSlotId(61);
	incompleteOptions.tree.last_slot = PhysicalSlotId(61);
	const std::array<VectorPqPageMapping, 1> incomplete{{{1, *pageId}}};
	static_cast<void>(VectorPqMapping::build(trees, *metadata, incompleteMappingIndexId, incomplete, incompleteOptions,
											 LogSequenceNumber(8)));
	snapshot = metadata->openSnapshot();
	const IndexRootLocation incompleteRoot = metadata->findIndexRoot(snapshot, incompleteMappingIndexId);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, incompleteRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(9))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingKeyIsNotCanonical) {
	constexpr uint64_t wrongKeyMappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA3);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> pageId =
			VectorPqMapping::findPage(snapshot, trees, *metadata, published.manifest.pq_mapping_index_id, 1);
	ASSERT_TRUE(pageId.has_value());
	snapshot.release();
	std::vector<VectorPqPageMapping> mappings;
	for (uint64_t vectorId = 1; vectorId <= trainingRecords().size(); ++vectorId) {
		mappings.push_back({vectorId + 10U, *pageId});
	}
	VectorPqMappingBuildOptions options = rebuildOptions().mapping;
	options.tree.pool_owner_id = wrongKeyMappingIndexId;
	options.tree.first_slot = PhysicalSlotId(61);
	options.tree.last_slot = PhysicalSlotId(61);
	static_cast<void>(
			VectorPqMapping::build(trees, *metadata, wrongKeyMappingIndexId, mappings, options, LogSequenceNumber(8)));
	snapshot = metadata->openSnapshot();
	const IndexRootLocation wrongKeyRoot = metadata->findIndexRoot(snapshot, wrongKeyMappingIndexId);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, wrongKeyRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(9))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingTargetsAZeroPageId) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	static_cast<void>(
			trees.upsert(published.manifest.pq_mapping_index_id, VectorPqMapping::key(1), 0U, LogSequenceNumber(8)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation replacementRoot = metadata->findIndexRoot(snapshot, published.manifest.pq_mapping_index_id);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, replacementRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(9))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingTargetsAnotherIndex) {
	constexpr uint64_t foreignMappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA4);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const std::array<PqVectorMaterializationRecord, 5> records{
			{{1, {0, 0}}, {2, {0, 0}}, {3, {0, 0}}, {4, {0, 0}}, {5, {0, 0}}}};
	const VectorPqMaterializationResult pq =
			vectors.materializePq(18, records, rebuildOptions().pages, LogSequenceNumber(4));
	VectorPqMappingBuildOptions options = rebuildOptions().mapping;
	options.tree.pool_owner_id = foreignMappingIndexId;
	options.tree.first_slot = PhysicalSlotId(61);
	options.tree.last_slot = PhysicalSlotId(61);
	static_cast<void>(VectorPqMapping::build(trees, *metadata, foreignMappingIndexId, pq.mappings, options,
											 LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation foreignRoot = metadata->findIndexRoot(snapshot, foreignMappingIndexId);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, foreignRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RefusesPqReclamationWhenAMappingTargetsALegacyPqPage) {
	constexpr uint64_t legacyMappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA5);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const std::array<PqVectorMaterializationRecord, 6> records{
			{{1, {0, 0}}, {2, {0, 0}}, {3, {0, 0}}, {4, {0, 0}}, {5, {0, 0}}, {99, {0, 0}}}};
	const VectorPqMaterializationResult pq =
			vectors.materializePq(17, records, rebuildOptions().pages, LogSequenceNumber(4));
	ASSERT_EQ(pq.pages.size(), 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorPage page = vectors.read(snapshot, pq.pages.front().page_id);
	const std::optional<uint16_t> legacySlot = page.findVectorId(99);
	ASSERT_TRUE(legacySlot.has_value());
	const VectorLocation legacyLocation{99, pq.pages.front().page_id, *legacySlot, page.slotGeneration(*legacySlot)};
	snapshot.release();
	static_cast<void>(metadata->upsertVectorLocations(std::span(&legacyLocation, 1), {}, LogSequenceNumber(5)));
	std::vector<VectorPqPageMapping> mappings;
	for (uint64_t vectorId = 1; vectorId <= trainingRecords().size(); ++vectorId) {
		mappings.push_back({vectorId, pq.pages.front().page_id});
	}
	VectorPqMappingBuildOptions options = rebuildOptions().mapping;
	options.tree.pool_owner_id = legacyMappingIndexId;
	options.tree.first_slot = PhysicalSlotId(61);
	options.tree.last_slot = PhysicalSlotId(61);
	static_cast<void>(
			VectorPqMapping::build(trees, *metadata, legacyMappingIndexId, mappings, options, LogSequenceNumber(6)));
	snapshot = metadata->openSnapshot();
	const IndexRootLocation legacyRoot = metadata->findIndexRoot(snapshot, legacyMappingIndexId);
	snapshot.release();
	pointManifestAtPqMapping(*metadata, manifests, 17, legacyRoot);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq({16, 16, 16, 16, 16}, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RetainsNonPqManifestEntriesWhenPublishingTheDerivedGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests, true);

	const OfflineVectorPqRebuildResult result = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	ASSERT_EQ(result.manifest.entries.size(), 2U);
	EXPECT_EQ(result.manifest.entries.front(), (VectorManifestEntry{PageId(900), VectorStorageKind::VSK_RAW, 1}));
	EXPECT_EQ(result.manifest.entries.back().storage_kind, VectorStorageKind::VSK_PQ);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, BeforePublicationFailureLeavesNoCandidateReachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorPqRebuildOptions interrupted = rebuildOptions();
	interrupted.manifest_publish_options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															   trainingRecords(), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopened);
	VectorManifestStore reopenedManifests(io, *reopened);
	SlottedPagePool reopenedSlotted(io, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopened);
	MetadataSnapshotPin interruptedSnapshot = reopened->openSnapshot();
	const VectorManifest interruptedManifest = reopenedManifests.read(interruptedSnapshot, 17).manifest();
	EXPECT_EQ(interruptedManifest.codebook_id, 0U);
	EXPECT_FALSE(interruptedManifest.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(interruptedManifest.entries.empty());
	EXPECT_FALSE(reopened->findExtentLocationOptional(
								 interruptedSnapshot,
								 VectorCodebookExtentIdCodec::manifestExtentId(rebuildOptions().codebook.codebook_id))
						 .has_value());
	EXPECT_FALSE(reopened->findIndexRootOptional(interruptedSnapshot, rebuildOptions().mapping_index_id).has_value());
	EXPECT_TRUE(reopened->findCurrentVectorPqPagePools(interruptedSnapshot).empty());
	interruptedSnapshot.release();
	const OfflineVectorPqRebuildResult resumed =
			OfflineVectorPqRebuild::run(reopenedTrees, reopenedVectors, reopenedCodebooks, reopenedManifests, *reopened,
										17, trainingRecords(), rebuildOptions());
	EXPECT_FALSE(resumed.reused_codebook);
	EXPECT_FALSE(resumed.reused_mapping);
	EXPECT_FALSE(resumed.already_published);
	EXPECT_EQ(resumed.manifest.pq_mapping_index_id, rebuildOptions().mapping_index_id);

	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedManifests.read(snapshot, 17).manifest(), resumed.manifest);
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, AfterPublicationFailureMakesTheCompleteCandidateReachable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorPqRebuildOptions interrupted = rebuildOptions();
	interrupted.manifest_publish_options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															   trainingRecords(), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopened);
	VectorManifestStore reopenedManifests(io, *reopened);
	SlottedPagePool reopenedSlotted(io, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	PhysicalSegmentAllocator reopenedSegments(io, *reopened);
	VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	const VectorManifest visible = reopenedManifests.read(snapshot, 17).manifest();
	EXPECT_EQ(visible.codebook_id, rebuildOptions().codebook.codebook_id);
	EXPECT_TRUE(visible.pq_mapping_root_page_id.isValid());
	EXPECT_TRUE(reopened->findExtentLocationOptional(snapshot,
													 VectorCodebookExtentIdCodec::manifestExtentId(visible.codebook_id))
						.has_value());
	EXPECT_TRUE(reopened->findIndexRootOptional(snapshot, visible.pq_mapping_index_id).has_value());
	ASSERT_FALSE(reopened->findCurrentVectorPqPagePools(snapshot).empty());
	const std::optional<PageId> mapped =
			VectorPqMapping::findPage(snapshot, reopenedTrees, *reopened, visible.pq_mapping_index_id, 3U);
	ASSERT_TRUE(mapped.has_value());
	const VectorPage page = reopenedVectors.read(snapshot, *mapped);
	EXPECT_TRUE(page.findVectorId(3U).has_value());
	EXPECT_EQ(reopenedCodebooks.read(snapshot, visible.codebook_id).header.generation,
			  rebuildOptions().codebook.generation);
	snapshot.release();

	const OfflineVectorPqRebuildResult retry =
			OfflineVectorPqRebuild::run(reopenedTrees, reopenedVectors, reopenedCodebooks, reopenedManifests, *reopened,
										17, trainingRecords(), rebuildOptions());
	EXPECT_TRUE(retry.reused_codebook);
	EXPECT_TRUE(retry.reused_mapping);
	EXPECT_TRUE(retry.already_published);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsATrainingInputThatDoesNotMatchDurableRawVectorSemantics) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	std::vector<DeterministicPqTrainingRecord> mismatched = trainingRecords();
	mismatched.front().raw_bfloat16 = bfloat16({1.0F, 0.0F, 10.0F, 10.0F});
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  mismatched, rebuildOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(
																		rebuildOptions().codebook.codebook_id))
						 .has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, rebuildOptions().mapping_index_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsInvalidBoundedOptionsBeforeWritingDerivedObjects) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorPqRebuildOptions invalid = rebuildOptions();
	invalid.pages.maximum_pages = 0;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = rebuildOptions();
	invalid.pages.maximum_pages = RSS_VECTOR_MANIFEST_MAX_ENTRIES + 1U;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = rebuildOptions();
	invalid.pages.maximum_records = std::numeric_limits<size_t>::max();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = rebuildOptions();
	invalid.pages.maximum_pages = std::numeric_limits<size_t>::max();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = rebuildOptions();
	invalid.mapping.maximum_entries = std::numeric_limits<size_t>::max();
	invalid.mapping.tree.maximum_entries = invalid.mapping.maximum_entries;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = rebuildOptions();
	invalid.mapping.tree.maximum_pages = std::numeric_limits<size_t>::max();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), invalid));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(
																		rebuildOptions().codebook.codebook_id))
						 .has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, rebuildOptions().mapping_index_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsPqPageBudgetBeforePublishingAnyDerivedObject) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);

	constexpr uint32_t dimensions = 64U;
	const size_t pqSlotsPerPage = VectorPageStore::rawSlotCapacity(sizeof(uint64_t) + dimensions);
	const std::vector<DeterministicPqTrainingRecord> training =
			trainingRecordsSpanningPqPages(pqSlotsPerPage + 1U, dimensions);
	materializeRawAndManifest(*metadata, vectors, manifests, training,
							 {PhysicalSlotId(40U), PhysicalSlotId(44U), training.size(), 4U});
	OfflineVectorPqRebuildOptions options = rebuildOptions();
	options.training = {dimensions, dimensions, 1U, 1U, training.size(), training.size()};
	options.codebook.dimensions = dimensions;
	options.codebook.subspace_count = dimensions;
	options.codebook.centroid_count = 1U;
	options.pages.maximum_records = training.size();
	options.pages.maximum_pages = 1U;
	options.mapping.maximum_entries = training.size();
	options.mapping.tree.maximum_entries = training.size();
	options.mapping.tree.maximum_pages = 16U;

	expectFormatError(
			[&] {
				static_cast<void>(
						OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U, training, options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(
					 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(options.codebook.codebook_id))
					 .has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, options.mapping_index_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, PublishesWhenTheCodebookWasPreparedBeforeTheRebuild) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	const DeterministicProductQuantizer quantizer =
			DeterministicProductQuantizer::train(trainingRecords(), options.training);
	static_cast<void>(codebooks.writeImmutable(options.codebook, quantizer.codebookPayload(), options.codebook_first_slot,
												 options.codebook_last_slot, LogSequenceNumber(4U)));

	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), options);
	EXPECT_TRUE(published.reused_codebook);
	EXPECT_FALSE(published.reused_mapping);
	EXPECT_FALSE(published.already_published);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsMappingTreePageBudgetBeforePublishingAnyDerivedObject) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);

	constexpr uint32_t dimensions = 64U;
	const std::vector<DeterministicPqTrainingRecord> training = trainingRecordsSpanningPqPages(1000U, dimensions);
	materializeRawAndManifest(*metadata, vectors, manifests, training,
							 {PhysicalSlotId(40U), PhysicalSlotId(44U), training.size(), 64U});
	OfflineVectorPqRebuildOptions options = rebuildOptions();
	options.training = {dimensions, dimensions, 1U, 1U, training.size(), training.size()};
	options.codebook.dimensions = dimensions;
	options.codebook.subspace_count = dimensions;
	options.codebook.centroid_count = 1U;
	options.pages.maximum_records = training.size();
	options.pages.maximum_pages = 64U;
	options.mapping.maximum_entries = training.size();
	options.mapping.tree.maximum_entries = training.size();
	options.mapping.tree.maximum_pages = 1U;

	expectFormatError(
			[&] {
				static_cast<void>(
						OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U, training, options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findExtentLocationOptional(
					 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(options.codebook.codebook_id))
					 .has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, options.mapping_index_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAVisibleMappingWithAZeroTargetPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	static_cast<void>(
			OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), options));
	static_cast<void>(trees.upsert(options.mapping_index_id, VectorPqMapping::key(1U), 0U, LogSequenceNumber(8U)));

	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
																			 trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAnExistingCodebookWithDifferentDeterministicContents) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	const OfflineVectorPqRebuildOptions incompatible = rebuildOptions();
	const DeterministicProductQuantizer quantizer =
			DeterministicProductQuantizer::train(trainingRecords(), incompatible.training);
	std::vector<std::byte> differentPayload = quantizer.codebookPayload();
	differentPayload.front() ^= std::byte{1};
	static_cast<void>(codebooks.writeImmutable(incompatible.codebook, differentPayload,
											   incompatible.codebook_first_slot, incompatible.codebook_last_slot,
											   LogSequenceNumber(4)));

	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), rebuildOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAnExistingCodebookWithADifferentPayloadShape) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);

	OfflineVectorPqRebuildOptions incompatible = rebuildOptions();
	incompatible.training.centroid_count = 1;
	incompatible.codebook.centroid_count = 1;
	const DeterministicProductQuantizer quantizer =
			DeterministicProductQuantizer::train(trainingRecords(), incompatible.training);
	static_cast<void>(codebooks.writeImmutable(incompatible.codebook, quantizer.codebookPayload(),
											   incompatible.codebook_first_slot, incompatible.codebook_last_slot,
											   LogSequenceNumber(4)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), rebuildOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, ReplacesAnyPriorPqManifestInventory) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests, false, true);
	const OfflineVectorPqRebuildResult result = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	ASSERT_EQ(result.manifest.entries.size(), 1U);
	EXPECT_NE(result.manifest.entries.front().page_id, PageId(901));
	EXPECT_EQ(result.manifest.entries.front().storage_kind, VectorStorageKind::VSK_PQ);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RetainsNonPqManifestEntriesAcrossAPublishedGenerationReplacement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests, true);

	const OfflineVectorPqRebuildOptions initial = rebuildOptions();
	static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
												  trainingRecords(), initial));
	const OfflineVectorPqRebuildResult replacement = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), replacementRebuildOptions());

	ASSERT_EQ(replacement.manifest.entries.size(), 2U);
	const auto raw = std::ranges::find_if(replacement.manifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_RAW;
	});
	ASSERT_NE(raw, replacement.manifest.entries.end());
	EXPECT_EQ(raw->page_id, PageId(900U));
	EXPECT_EQ(raw->record_count, 1U);
	EXPECT_TRUE(std::ranges::any_of(replacement.manifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	}));
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAnAlreadyPublishedMappingWhosePqCodesWereCorrupted) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
												  trainingRecords(), options));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> pageId =
			VectorPqMapping::findPage(snapshot, trees, *metadata, options.mapping_index_id, 1U);
	ASSERT_TRUE(pageId.has_value());
	const VectorPage page = vectors.read(snapshot, *pageId);
	const std::optional<uint16_t> slot = page.findVectorId(1U);
	ASSERT_TRUE(slot.has_value());
	std::vector<std::byte> corrupt = page.read(*slot);
	ASSERT_GT(corrupt.size(), sizeof(uint64_t));
	corrupt[sizeof(uint64_t)] ^= std::byte{0x01};
	snapshot.release();
	overwriteVectorPageRecord(*metadata, vectors, *pageId, 1U, corrupt);

	// Reopen with a fresh metadata/cache instance so the assertion observes the
	// coherently checksummed on-disk corruption rather than a stale page view.
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopenedMetadata);
	VectorManifestStore reopenedManifests(io, *reopenedMetadata);
	SlottedPagePool reopenedSlotted(io, *reopenedMetadata);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopenedMetadata);
	PhysicalSegmentAllocator reopenedSegments(io, *reopenedMetadata);
	VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopenedMetadata);
	MetadataSnapshotPin beforeRejected = reopenedMetadata->openSnapshot();
	const Generation generation = beforeRejected.superblock().generation;
	beforeRejected.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(reopenedTrees, reopenedVectors, reopenedCodebooks,
															  reopenedManifests, *reopenedMetadata, 17U,
															  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejected = reopenedMetadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generation);
	afterRejected.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsUnreachablePqManifestInventoryOnIdempotentRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	const OfflineVectorPqRebuildResult first = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), options);

	VectorManifest corrupt = first.manifest;
	// The persisted Manifest codec already rejects duplicate page IDs. Use a
	// canonical, distinct entry that no mapping reaches to test the rebuild
	// layer's independent inventory-to-mapping validation.
	corrupt.entries.push_back({PageId(UINT64_C(999999)), VectorStorageKind::VSK_PQ, 1U});
	overwriteManifest(*metadata, corrupt);

	// Reopen so the retry decodes the persisted Manifest rather than a cached page view.
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopenedMetadata);
	VectorManifestStore reopenedManifests(io, *reopenedMetadata);
	SlottedPagePool reopenedSlotted(io, *reopenedMetadata);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopenedMetadata);
	PhysicalSegmentAllocator reopenedSegments(io, *reopenedMetadata);
	VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopenedMetadata);
	MetadataSnapshotPin beforeRejected = reopenedMetadata->openSnapshot();
	const Generation generation = beforeRejected.superblock().generation;
	beforeRejected.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(reopenedTrees, reopenedVectors, reopenedCodebooks,
						reopenedManifests, *reopenedMetadata, 17U, trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejected = reopenedMetadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generation);
	afterRejected.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsPqManifestRecordCountMismatchOnIdempotentRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	const OfflineVectorPqRebuildResult first = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17U, trainingRecords(), options);

	VectorManifest corrupt = first.manifest;
	const auto pq = std::ranges::find_if(corrupt.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	});
	ASSERT_NE(pq, corrupt.entries.end());
	ASSERT_LT(pq->record_count, std::numeric_limits<uint32_t>::max());
	++pq->record_count;
	overwriteManifest(*metadata, corrupt);

	// The page and the mapping remain individually valid; only their persisted
	// accounting relationship is corrupt, so this must be rejected by the
	// rebuild's inventory authentication.
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedVectors(io, *reopenedMetadata);
	VectorManifestStore reopenedManifests(io, *reopenedMetadata);
	SlottedPagePool reopenedSlotted(io, *reopenedMetadata);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopenedMetadata);
	PhysicalSegmentAllocator reopenedSegments(io, *reopenedMetadata);
	VectorCodebookStore reopenedCodebooks(reopenedSegments, *reopenedMetadata);
	MetadataSnapshotPin beforeRejected = reopenedMetadata->openSnapshot();
	const Generation generation = beforeRejected.superblock().generation;
	beforeRejected.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(reopenedTrees, reopenedVectors, reopenedCodebooks,
						reopenedManifests, *reopenedMetadata, 17U, trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejected = reopenedMetadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generation);
	afterRejected.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAVisibleMappingWithDifferentRecordsOrCodes) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const OfflineVectorPqRebuildOptions stagedOptions = rebuildOptions();
	materializeRawAndManifest(*metadata, vectors, manifests);
	stageMapping(*metadata, vectors, trees, stagedOptions);

	OfflineVectorPqRebuildOptions shorter = stagedOptions;
	shorter.codebook.codebook_id = 72;
	shorter.codebook.manifest_segment_id = SegmentId(72);
	shorter.codebook_first_slot = PhysicalSlotId(61);
	shorter.codebook_last_slot = PhysicalSlotId(61);
	std::vector<DeterministicPqTrainingRecord> four = trainingRecords();
	four.pop_back();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17, four,
															  shorter));
			},
			FormatError::FERR_INVALID_STATE);

	OfflineVectorPqRebuildOptions differentCodes = stagedOptions;
	differentCodes.codebook.codebook_id = 73;
	differentCodes.codebook.manifest_segment_id = SegmentId(73);
	differentCodes.codebook_first_slot = PhysicalSlotId(62);
	differentCodes.codebook_last_slot = PhysicalSlotId(62);
	differentCodes.training.centroid_count = 1;
	differentCodes.codebook.centroid_count = 1;
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), differentCodes));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin mappingSnapshot = metadata->openSnapshot();
	const std::optional<PageId> replacementPage =
			VectorPqMapping::findPage(mappingSnapshot, trees, *metadata, stagedOptions.mapping_index_id, 2);
	mappingSnapshot.release();
	ASSERT_TRUE(replacementPage.has_value());
	ASSERT_TRUE(trees.erase(stagedOptions.mapping_index_id, VectorPqMapping::key(1), LogSequenceNumber(8)));
	static_cast<void>(trees.upsert(stagedOptions.mapping_index_id, VectorPqMapping::key(9), replacementPage->value(),
								   LogSequenceNumber(9)));
	OfflineVectorPqRebuildOptions differentIds = stagedOptions;
	differentIds.codebook.codebook_id = 74;
	differentIds.codebook.manifest_segment_id = SegmentId(74);
	differentIds.codebook_first_slot = PhysicalSlotId(63);
	differentIds.codebook_last_slot = PhysicalSlotId(63);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), differentIds));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAVisibleMappingWithDifferentVectorIds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	materializeRawAndManifest(*metadata, vectors, manifests);
	stageMapping(*metadata, vectors, trees, options);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> replacement =
			VectorPqMapping::findPage(snapshot, trees, *metadata, options.mapping_index_id, 2);
	snapshot.release();
	ASSERT_TRUE(replacement.has_value());
	ASSERT_TRUE(trees.erase(options.mapping_index_id, VectorPqMapping::key(1), LogSequenceNumber(6)));
	static_cast<void>(trees.upsert(options.mapping_index_id, VectorPqMapping::key(9), replacement->value(),
								   LogSequenceNumber(7)));

	MetadataSnapshotPin changedSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(VectorPqMapping::findPage(changedSnapshot, trees, *metadata, options.mapping_index_id, 1).has_value());
	EXPECT_TRUE(VectorPqMapping::findPage(changedSnapshot, trees, *metadata, options.mapping_index_id, 9).has_value());
	changedSnapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsCheckpointLsnExhaustionBeforePublishingDerivedObjects) {
	Superblock nearExhaustion = initialSuperblock();
	nearExhaustion.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 2U);
	auto metadata = RelocatableMetadataStore::initializeNew(io, nearExhaustion);
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	std::vector<RawVectorMaterializationRecord> raw;
	for (const DeterministicPqTrainingRecord &record: trainingRecords()) {
		raw.push_back(rawRecord(record));
	}
	static_cast<void>(vectors.materializeRaw(17, raw, rawOptions(),
											 LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 1U)));
	VectorManifest manifest;
	manifest.index_id = 17;
	manifest.dimensions = 4;
	static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45),
											LogSequenceNumber(std::numeric_limits<uint64_t>::max())));

	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), rebuildOptions()));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAPqMappingThatPointsAtARawVectorPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	materializeRawAndManifest(*metadata, vectors, manifests);
	stageMapping(*metadata, vectors, trees, options);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorLocation rawLocation = metadata->findVectorLocation(snapshot, 1);
	snapshot.release();
	static_cast<void>(trees.upsert(options.mapping_index_id, VectorPqMapping::key(1), rawLocation.page_id.value(),
								   LogSequenceNumber(6)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAPqMappingThatDoesNotContainTheMappedVectorId) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	materializeRawAndManifest(*metadata, vectors, manifests);
	stageMapping(*metadata, vectors, trees, options);
	const std::array<PqVectorMaterializationRecord, 1> unrelated{{{99, {0, 0}}}};
	const VectorPqMaterializationResult unrelatedPages =
			vectors.materializePq(17, unrelated, options.pages, LogSequenceNumber(6));
	static_cast<void>(trees.upsert(options.mapping_index_id, VectorPqMapping::key(1),
								   unrelatedPages.pages.front().page_id.value(), LogSequenceNumber(7)));
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsStoresBoundToDifferentMetadataInstances) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	auto foreignMetadata = RelocatableMetadataStore::open(io);
	VectorPageStore foreignVectors(io, *foreignMetadata);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, foreignVectors, codebooks, manifests, *metadata,
															  17, trainingRecords(), rebuildOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAVisibleManifestWithTheWrongMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildResult published = OfflineVectorPqRebuild::run(
			trees, vectors, codebooks, manifests, *metadata, 17, trainingRecords(), rebuildOptions());
	VectorManifest corrupt = published.manifest;
	corrupt.pq_mapping_root_page_id = PageId(999);
	overwriteManifest(*metadata, corrupt);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17,
															  trainingRecords(), rebuildOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsAnUnanchoredManifestThatClaimsAPqMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();

	MetadataSnapshotPin manifestSnapshot = metadata->openSnapshot();
	VectorManifest corrupt = manifests.read(manifestSnapshot, 17U).manifest();
	manifestSnapshot.release();
	corrupt.pq_mapping_index_id = options.mapping_index_id;
	corrupt.pq_mapping_root_page_id = PageId(999U);
	corrupt.pq_mapping_root_generation = Generation(1U);
	// This is a checksummed on-disk Manifest corruption: no mapping root has
	// ever been published, so rebuild must not treat its PQ inventory as a base.
	overwriteManifest(*metadata, corrupt);

	MetadataSnapshotPin beforeRejected = metadata->openSnapshot();
	const Generation generation = beforeRejected.superblock().generation;
	beforeRejected.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
																								  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejected = metadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generation);
	afterRejected.release();
}

TEST_F(RelocatableOfflineVectorPqRebuildTest, RejectsPublishedPqMappingWithMissingCanonicalVector) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	PhysicalSegmentAllocator segments(io, *metadata);
	VectorCodebookStore codebooks(segments, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorPqRebuildOptions options = rebuildOptions();
	static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
																					  trainingRecords(), options));

	ASSERT_TRUE(trees.erase(options.mapping_index_id, VectorPqMapping::key(1U), LogSequenceNumber(8U)));
	MetadataSnapshotPin beforeRejection = metadata->openSnapshot();
	const Generation generationBeforeRejection = beforeRejection.superblock().generation;
	beforeRejection.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineVectorPqRebuild::run(trees, vectors, codebooks, manifests, *metadata, 17U,
															  trainingRecords(), options));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejected = metadata->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, generationBeforeRejection);
	afterRejected.release();
}
