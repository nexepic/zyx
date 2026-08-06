/**
 * @file test_DerivedObjectReachabilityReclaimer.cpp
 * @brief Conservative derived-object reachability reclamation tests.
 */

#include <bit>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x91);
	constexpr uint64_t WRONG_KEY_MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x92);
	constexpr uint64_t FOREIGN_PAGE_MAPPING_INDEX_ID =
			RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x93);
	constexpr uint64_t STALE_PQ_MAPPING_INDEX_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA1);
	constexpr uint64_t VISIBLE_PQ_MAPPING_INDEX_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA2);
	constexpr uint64_t STALE_ADJACENCY_MAPPING_INDEX_ID =
			RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA3);
	constexpr uint64_t LARGE_PQ_MAPPING_INDEX_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0xA4);
	constexpr uint64_t FUTURE_HIGH_BIT_INDEX_ID = UINT64_C(0xA503000000000001);

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

	RawVectorMaterializationRecord rawRecord(const CanonicalVectorRecord &record) {
		RawVectorMaterializationRecord result;
		result.vector_id = record.vector_id;
		result.bytes.resize(sizeof(uint64_t) + record.raw_bfloat16.size());
		LittleEndianCodec::writeU64(result.bytes, 0, record.vector_id);
		std::copy(record.raw_bfloat16.begin(), record.raw_bfloat16.end(), result.bytes.begin() + sizeof(uint64_t));
		return result;
	}

	OfflineVectorAdjacencyRebuildOptions rebuildOptions() {
		OfflineVectorAdjacencyRebuildOptions result;
		result.maximum_exact_records = 16;
		result.maximum_degree = 2;
		result.adjacency_generation = Generation(1);
		result.pages = {PhysicalSlotId(51), PhysicalSlotId(52), 16, 8};
		result.mapping_index_id = MAPPING_INDEX_ID;
		result.mapping.tree = {PhysicalSlotId(53), PhysicalSlotId(54), MAPPING_INDEX_ID, 16, 4};
		result.mapping.maximum_entries = 16;
		return result;
	}

	VectorAdjacencyMaterializationOptions stagedOptions() {
		return {PhysicalSlotId(61), PhysicalSlotId(61), 16, 4};
	}

	DerivedObjectReachabilityReclaimOptions reclaimOptions() { return {16, 16, 16, 16, 16, 16, 16}; }

	VectorPqMappingBuildOptions pqMappingOptions(uint64_t mappingIndexId, PhysicalSlotId slot,
											 size_t maximumEntries = 16U, size_t maximumPages = 4U) {
		VectorPqMappingBuildOptions result;
		result.tree = {slot, slot, mappingIndexId, maximumEntries, maximumPages};
		result.maximum_entries = maximumEntries;
		return result;
	}

	VectorAdjacencyMappingBuildOptions mappingOptions(uint64_t mappingIndexId, PhysicalSlotId slot) {
		VectorAdjacencyMappingBuildOptions result;
		result.tree = {slot, slot, mappingIndexId, 16, 4};
		result.maximum_entries = 16;
		return result;
	}

	std::vector<VectorPqPageMapping> pqMappings(size_t count) {
		std::vector<VectorPqPageMapping> result;
		result.reserve(count);
		for (size_t index = 1U; index <= count; ++index) {
			result.push_back({index, PageId(1000U + index)});
		}
		return result;
	}

	std::vector<VectorAdjacencyPageMapping> adjacencyMappings(size_t count) {
		std::vector<VectorAdjacencyPageMapping> result;
		result.reserve(count);
		for (size_t index = 1U; index <= count; ++index) {
			result.push_back({index, PageId(2000U + index)});
		}
		return result;
	}

	size_t pqMappingCountRequiringMultiplePages() {
		constexpr size_t packedEntryCapacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
											   2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
		std::vector<BTreePageEntry> entries;
		for (uint64_t vectorId = 1U;; ++vectorId) {
			entries.push_back({VectorPqMapping::key(vectorId), PageId(vectorId).value()});
			if (PackedBTreePageCodec::encode(BTreeNodeKind::BPN_LEAF, entries).size() > packedEntryCapacity) {
				return static_cast<size_t>(vectorId);
			}
			if (vectorId == std::numeric_limits<uint64_t>::max()) {
				throw std::runtime_error("B+Tree leaf capacity cannot be exceeded safely");
			}
		}
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

	void buildNonDerivedRoot(BTreePageStore &trees, RelocatableMetadataStore &metadata, uint64_t indexId,
							 PhysicalSlotId slot, LogSequenceNumber checkpointLsn) {
		const BulkBTreeBuildOptions options{slot, slot, indexId, 1U, 1U};
		static_cast<void>(BulkBTreeBuilder::build(
				trees, metadata, indexId, orderedSource({{{std::byte{1}}, UINT64_C(1)}}), options, checkpointLsn));
	}

	void bindAdjacencyMapping(RelocatableMetadataStore &metadata, VectorManifestStore &manifests, uint64_t indexId,
							  const IndexRootLocation &mappingRoot, Generation adjacencyGeneration,
							  LogSequenceNumber checkpointLsn) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const VectorManifest current = manifests.read(snapshot, indexId).manifest();
		const PageLocation location = metadata.findPageLocation(snapshot, current.page_id);
		snapshot.release();
		VectorManifest candidate = current;
		candidate.entry_point_vector_id = 1;
		candidate.adjacency_generation = adjacencyGeneration;
		candidate.adjacency_mapping_index_id = mappingRoot.index_id;
		candidate.adjacency_mapping_root_page_id = mappingRoot.root_page_id;
		candidate.adjacency_mapping_root_generation = mappingRoot.root_generation;
		candidate.committed_lsn = checkpointLsn;
		const PageOwnerHeader owner{candidate.page_id,
								candidate.index_id,
								PageOwnerKind::POK_VECTOR_MANIFEST,
								0,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								candidate.generation,
								checkpointLsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, candidate);
		static_cast<void>(manifests.replace(page, location.segment_id, current.committed_lsn, checkpointLsn));
	}

	void bindPqMapping(RelocatableMetadataStore &metadata, VectorManifestStore &manifests, uint64_t indexId,
					   const IndexRootLocation &mappingRoot, LogSequenceNumber checkpointLsn) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const VectorManifest current = manifests.read(snapshot, indexId).manifest();
		const PageLocation location = metadata.findPageLocation(snapshot, current.page_id);
		snapshot.release();
		VectorManifest candidate = current;
		candidate.pq_mapping_index_id = mappingRoot.index_id;
		candidate.pq_mapping_root_page_id = mappingRoot.root_page_id;
		candidate.pq_mapping_root_generation = mappingRoot.root_generation;
		candidate.committed_lsn = checkpointLsn;
		const PageOwnerHeader owner{candidate.page_id,
									candidate.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									candidate.generation,
									checkpointLsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, candidate);
		static_cast<void>(manifests.replace(page, location.segment_id, current.committed_lsn, checkpointLsn));
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

	void overwritePageOwner(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							const PageOwnerHeader &owner) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, bytes.data(), bytes.size());
	}

	std::vector<VectorAdjacencyMaterializationRecord> stagedRecords() {
		std::vector<VectorAdjacencyMaterializationRecord> result;
		for (const CanonicalVectorRecord &record: canonicalRecords()) {
			result.push_back({record.vector_id, {}});
		}
		return result;
	}

} // namespace

class RelocatableDerivedObjectReachabilityReclaimerTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_derived_reclaimer_" + to_string(uuid));
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
								   VectorManifestStore &manifests) {
		std::vector<RawVectorMaterializationRecord> raw;
		for (const CanonicalVectorRecord &record: canonicalRecords()) {
			raw.push_back(rawRecord(record));
		}
		static_cast<void>(vectors.materializeRaw(17, raw, {PhysicalSlotId(40), PhysicalSlotId(40), 16, 4},
																	  LogSequenceNumber(2)));
		VectorManifest manifest;
		manifest.index_id = 17;
		manifest.dimensions = 2;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(3)));
	}
};

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, ReclaimsOnlyUnreachableDerivedPagesAndPreservesPinnedReads) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const OfflineVectorAdjacencyRebuildResult graph = OfflineVectorAdjacencyRebuild::run(
			trees, vectors, adjacencies, manifests, *metadata, 17, canonicalRecords(), rebuildOptions());
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(7));
	ASSERT_EQ(staged.pages.size(), 1U);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const PageLocation stagedLocation = metadata->findPageLocation(pinned, staged.pages.front().page_id);
	const std::optional<PageId> visiblePage =
			VectorAdjacencyMapping::findPage(pinned, trees, *metadata, graph.manifest.adjacency_mapping_index_id, 1);
	ASSERT_TRUE(visiblePage.has_value());
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report = reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(8));
	EXPECT_EQ(report.scanned_generation, Generation(7));
	EXPECT_EQ(report.published_generation, Generation(8));
	EXPECT_EQ(report.reachable_adjacency_pages, graph.manifest.entry_point_vector_id == 0U ? 0U : 1U);
	EXPECT_EQ(report.derived_adjacency_pages_retired, staged.pages.size());
	EXPECT_EQ(adjacencies.readRecord(pinned, staged.pages.front().page_id, 1), (VectorAdjacencyRecord{1, {}}));
	EXPECT_EQ(adjacencies.readRecord(pinned, *visiblePage, 1).vector_id, 1U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, staged.pages.front().page_id).has_value());
	EXPECT_EQ(adjacencies.readRecord(current, *visiblePage, 1).vector_id, 1U);
	current.release();
	pinned.release();
	adjacencies.reclaimRetiredFrames(stagedLocation.segment_id);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsAStaleRetirementPlanWithoutChangingReachability) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	const VectorAdjacencyMaterializationResult first =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(2));
	MetadataSnapshotPin planSnapshot = metadata->openSnapshot();
	const std::optional<DerivedVectorAdjacencyPage> observed =
			adjacencies.inspectDerivedPage(planSnapshot, first.pages.front().page_id);
	ASSERT_TRUE(observed.has_value());
	const Generation planGeneration = planSnapshot.superblock().generation;
	planSnapshot.release();
	VectorAdjacencyMaterializationOptions next = stagedOptions();
	next.first_slot = PhysicalSlotId(62);
	next.last_slot = PhysicalSlotId(62);
	static_cast<void>(adjacencies.materialize(17, stagedRecords(), next, LogSequenceNumber(3)));
	const std::array<DerivedVectorAdjacencyPage, 1> plan{*observed};
	expectFormatError(
			[&] {
				static_cast<void>(adjacencies.retireDerivedPages(planGeneration, plan, LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findPageLocationOptional(current, first.pages.front().page_id).has_value());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, InspectsUnknownNamespacesConservativelyAndRejectsRetiredOwners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const SlottedPage page = adjacencies.readPage(snapshot, staged.pages.front().page_id);
	const PageLocation location = metadata->findPageLocation(snapshot, staged.pages.front().page_id);
	snapshot.release();
	const PageOwnerHeader unknownOwner{location.page_id,
										  99,
										  PageOwnerKind::POK_VECTOR_ADJACENCY,
										  0,
										  static_cast<uint32_t>(page.serialize().size()),
										  location.generation,
										  LogSequenceNumber(2)};
	overwritePageOwner(io, *metadata, location.page_id, unknownOwner);
	MetadataSnapshotPin unknownSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(adjacencies.inspectDerivedPage(unknownSnapshot, location.page_id).has_value());
	unknownSnapshot.release();

	PageOwnerHeader retiredOwner = unknownOwner;
	retiredOwner.owner_id = VectorAdjacencyPageStore::derivedOwnerId(17);
	retiredOwner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	overwritePageOwner(io, *metadata, location.page_id, retiredOwner);
	MetadataSnapshotPin retiredSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(adjacencies.inspectDerivedPage(retiredSnapshot, location.page_id)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsEmptyAndNoncanonicalDerivedRetirementPlansBeforeMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	const std::vector<DerivedVectorAdjacencyPage> noPages;
	expectFormatError(
			[&] { static_cast<void>(adjacencies.retireDerivedPages(Generation(1), noPages, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<DerivedVectorAdjacencyPage> observed =
			adjacencies.inspectDerivedPage(snapshot, staged.pages.front().page_id);
	ASSERT_TRUE(observed.has_value());
	DerivedVectorAdjacencyPage invalid = *observed;
	invalid.index_id = RSS_DERIVED_ADJACENCY_OWNER_MASK | UINT64_C(17);
	const std::array<DerivedVectorAdjacencyPage, 1> invalidPlan{invalid};
	expectFormatError(
			[&] {
				static_cast<void>(
						adjacencies.retireDerivedPages(snapshot.superblock().generation, invalidPlan, LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsStaleAndNoncanonicalPqRetirementPlansBeforeMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	const VectorPqMaterializationOptions options{PhysicalSlotId(40), PhysicalSlotId(41), Generation(1), 16, 4};
	const std::array<PqVectorMaterializationRecord, 1> firstRecords{{{1, {1, 2}}}};
	const std::vector<DerivedVectorPqPage> noPages;
	expectFormatError(
			[&] { static_cast<void>(vectors.retireDerivedPqPages(Generation(1), noPages, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	const VectorPqMaterializationResult first = vectors.materializePq(17, firstRecords, options, LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<DerivedVectorPqPage> observed = vectors.inspectDerivedPqPage(snapshot, first.pages.front().page_id);
	ASSERT_TRUE(observed.has_value());
	const Generation planGeneration = snapshot.superblock().generation;
	snapshot.release();
	const std::array<PqVectorMaterializationRecord, 1> secondRecords{{{2, {2, 1}}}};
	static_cast<void>(vectors.materializePq(17, secondRecords, options, LogSequenceNumber(3)));
	const std::array<DerivedVectorPqPage, 1> plan{*observed};
	expectFormatError(
			[&] { static_cast<void>(vectors.retireDerivedPqPages(planGeneration, plan, LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_STATE);
	DerivedVectorPqPage invalid = *observed;
	invalid.index_id = RSS_DERIVED_ADJACENCY_OWNER_MASK | UINT64_C(17);
	const std::array<DerivedVectorPqPage, 1> invalidPlan{invalid};
	expectFormatError(
			[&] { static_cast<void>(vectors.retireDerivedPqPages(Generation(3), invalidPlan, LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsUnboundedCleanupOptionsBeforeScanningStorage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions invalid = reclaimOptions();
	invalid.maximum_mapping_entries = 0;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(invalid, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = reclaimOptions();
	invalid.maximum_mapping_roots = 0;
	expectFormatError(
			[&] { static_cast<void>(reclaimer.reclaimMappingTrees(invalid, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	invalid = reclaimOptions();
	invalid.maximum_mapping_tree_pages = 0;
	expectFormatError(
			[&] { static_cast<void>(reclaimer.reclaimMappingTrees(invalid, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, DoesNotPublishWhenNoDerivedAdjacencyPageIsReachableOrStaged) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report = reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(2));
	EXPECT_EQ(report.scanned_generation, Generation(1));
	EXPECT_EQ(report.published_generation, Generation(1));
	EXPECT_EQ(report.derived_adjacency_pages_retired, 0U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, EnforcesPqDirectoryBoundsAndSkipsManifestsWithoutPqMappings) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_vector_locations = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq(limited, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
	const DerivedObjectReachabilityReclaimReport skipped = reclaimer.reclaimPq(reclaimOptions(), LogSequenceNumber(4));
	EXPECT_EQ(skipped.vector_locations, canonicalRecords().size());
	EXPECT_EQ(skipped.vector_manifests, 1U);
	EXPECT_EQ(skipped.published_generation, skipped.scanned_generation);

	VectorManifest other;
	other.index_id = 18;
	other.dimensions = 2;
	other.metric = VectorDistanceMetric::VDM_L2;
	static_cast<void>(manifests.materialize(other, PhysicalSlotId(46), PhysicalSlotId(46), LogSequenceNumber(5)));
	limited = reclaimOptions();
	limited.maximum_vector_manifests = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq(limited, LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsPageStoresAttachedToAnotherMetadataInstance) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	{
		auto foreign = RelocatableMetadataStore::open(io);
		VectorPageStore foreignVectors(io, *foreign);
		VectorAdjacencyPageStore foreignAdjacencies(io, *foreign);
		VectorManifestStore foreignManifests(io, *foreign);
		SlottedPagePool foreignSlotted(io, *foreign);
		BTreePageStore foreignTrees(foreignSlotted, *foreign);
		EXPECT_THROW(DerivedObjectReachabilityReclaimer(foreignTrees, vectors, adjacencies, manifests, *metadata),
					 std::invalid_argument);
		EXPECT_THROW(DerivedObjectReachabilityReclaimer(trees, foreignVectors, adjacencies, manifests, *metadata),
					 std::invalid_argument);
		EXPECT_THROW(DerivedObjectReachabilityReclaimer(trees, vectors, foreignAdjacencies, manifests, *metadata),
					 std::invalid_argument);
		EXPECT_THROW(DerivedObjectReachabilityReclaimer(trees, vectors, adjacencies, foreignManifests, *metadata),
					 std::invalid_argument);
	}
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, FailsClosedAtTheAdjacencyMappingBoundAndReleasesItsScanPin) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
															 canonicalRecords(), rebuildOptions()));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_mapping_entries = 4;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(limited, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, EnforcesRawManifestCandidateAndRetirementBounds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	std::vector<RawVectorMaterializationRecord> raw;
	for (const CanonicalVectorRecord &record: canonicalRecords()) {
		raw.push_back(rawRecord(record));
	}
	static_cast<void>(vectors.materializeRaw(17, raw, {PhysicalSlotId(40), PhysicalSlotId(40), 16, 4},
																	  LogSequenceNumber(2)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_vector_locations = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(limited, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);

	for (const uint64_t indexId: {UINT64_C(17), UINT64_C(18)}) {
		VectorManifest manifest;
		manifest.index_id = indexId;
		manifest.dimensions = 2;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		static_cast<void>(
				manifests.materialize(manifest, PhysicalSlotId(45 + indexId - 17U), PhysicalSlotId(45 + indexId - 17U),
															LogSequenceNumber(indexId - 14U)));
	}
	limited = reclaimOptions();
	limited.maximum_vector_manifests = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(limited, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);

	std::vector<VectorAdjacencyMaterializationRecord> multiPage;
	for (uint64_t vectorId = 1; vectorId <= 6; ++vectorId) {
		std::vector<uint64_t> neighbors;
		for (uint64_t neighbor = 1000; neighbor < 1150; ++neighbor) {
			neighbors.push_back(neighbor);
		}
		multiPage.push_back({vectorId, std::move(neighbors)});
	}
	VectorAdjacencyMaterializationOptions pages{PhysicalSlotId(61), PhysicalSlotId(61), 16, 8};
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, multiPage, pages, LogSequenceNumber(5));
	ASSERT_GT(staged.pages.size(), 1U);
	limited = reclaimOptions();
	limited.maximum_candidate_pages = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(limited, LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	limited = reclaimOptions();
	limited.maximum_retirements = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(limited, LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
	const DerivedObjectReachabilityReclaimReport completed = reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(6));
	EXPECT_EQ(completed.derived_adjacency_pages_retired, staged.pages.size());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RefusesToReclaimWhenAManifestMappingDoesNotCoverRawVectors) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(4));
	VectorAdjacencyMappingBuildOptions mapping;
	mapping.tree = {PhysicalSlotId(62), PhysicalSlotId(62), MAPPING_INDEX_ID, 16, 4};
	mapping.maximum_entries = 16;
	const std::array<VectorAdjacencyPageMapping, 1> incomplete{staged.mappings.front()};
	static_cast<void>(
			VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, incomplete, mapping, LogSequenceNumber(5)));
	MetadataSnapshotPin beforeManifest = metadata->openSnapshot();
	const VectorManifest current = manifests.read(beforeManifest, 17).manifest();
	const PageLocation location = metadata->findPageLocation(beforeManifest, current.page_id);
	const IndexRootLocation root = metadata->findIndexRoot(beforeManifest, MAPPING_INDEX_ID);
	beforeManifest.release();
	VectorManifest candidate = current;
	candidate.entry_point_vector_id = 1;
	candidate.adjacency_generation = Generation(1);
	candidate.adjacency_mapping_index_id = MAPPING_INDEX_ID;
	candidate.adjacency_mapping_root_page_id = root.root_page_id;
	candidate.adjacency_mapping_root_generation = root.root_generation;
	candidate.committed_lsn = LogSequenceNumber(6);
	const PageOwnerHeader owner{candidate.page_id,
								candidate.index_id,
								PageOwnerKind::POK_VECTOR_MANIFEST,
								0,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								candidate.generation,
								candidate.committed_lsn};
	const VectorManifestPage page = VectorManifestPage::create(owner, candidate);
	static_cast<void>(manifests.replace(page, location.segment_id, current.committed_lsn, candidate.committed_lsn));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findPageLocationOptional(snapshot, staged.pages.front().page_id).has_value());
	EXPECT_EQ(metadata->activeSnapshotCount(), 1U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RefusesANoncanonicalAdjacencyMappingKey) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(4));
	std::vector<VectorAdjacencyPageMapping> mappings;
	for (const VectorAdjacencyPageMapping &mapping: staged.mappings) {
		mappings.push_back({mapping.vector_id + 10U, mapping.page_id});
	}
	static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, WRONG_KEY_MAPPING_INDEX_ID, mappings,
															mappingOptions(WRONG_KEY_MAPPING_INDEX_ID, PhysicalSlotId(62)),
															LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, WRONG_KEY_MAPPING_INDEX_ID);
	snapshot.release();
	bindAdjacencyMapping(*metadata, manifests, 17, root, Generation(1), LogSequenceNumber(6));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RefusesAMappingToAnotherIndexesDerivedAdjacencyPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	const VectorAdjacencyMaterializationResult staged =
			adjacencies.materialize(18, stagedRecords(), stagedOptions(), LogSequenceNumber(4));
	static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, FOREIGN_PAGE_MAPPING_INDEX_ID, staged.mappings,
															mappingOptions(FOREIGN_PAGE_MAPPING_INDEX_ID, PhysicalSlotId(62)),
															LogSequenceNumber(5)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, FOREIGN_PAGE_MAPPING_INDEX_ID);
	snapshot.release();
	bindAdjacencyMapping(*metadata, manifests, 17, root, Generation(1), LogSequenceNumber(6));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RefusesAManifestWhoseAdjacencyRootDiffersFromTheIndexDirectory) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	materializeRawAndManifest(*metadata, vectors, manifests);
	static_cast<void>(OfflineVectorAdjacencyRebuild::run(trees, vectors, adjacencies, manifests, *metadata, 17,
															 canonicalRecords(), rebuildOptions()));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	VectorManifest corrupt = manifests.read(snapshot, 17).manifest();
	snapshot.release();
	corrupt.adjacency_mapping_root_generation =
			Generation(corrupt.adjacency_mapping_root_generation.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsPqLocationsInTheRawVectorDirectory) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::array<PqVectorMaterializationRecord, 1> records{{{99, {1, 2}}}};
	const VectorPqMaterializationOptions options{PhysicalSlotId(40), PhysicalSlotId(40), Generation(1), 16, 4};
	const VectorPqMaterializationResult pq = vectors.materializePq(17, records, options, LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorPage page = vectors.read(snapshot, pq.pages.front().page_id);
	const std::optional<uint16_t> slot = page.findVectorId(99);
	ASSERT_TRUE(slot.has_value());
	const VectorLocation location{99, pq.pages.front().page_id, *slot, page.slotGeneration(*slot)};
	snapshot.release();
	static_cast<void>(metadata->upsertVectorLocations(std::span(&location, 1), {}, LogSequenceNumber(3)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport adjacency = reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(4));
	EXPECT_EQ(adjacency.vector_locations, 1U);
	EXPECT_EQ(adjacency.derived_adjacency_pages_retired, 0U);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq(reclaimOptions(), LogSequenceNumber(4))); },
				  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RefusesARawVectorLocationWhoseRecordPrefixWasCorrupted) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	std::vector<RawVectorMaterializationRecord> raw;
	for (const CanonicalVectorRecord &record: canonicalRecords()) {
		raw.push_back(rawRecord(record));
	}
	static_cast<void>(vectors.materializeRaw(17, raw, {PhysicalSlotId(40), PhysicalSlotId(40), 16, 4},
																	  LogSequenceNumber(2)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const VectorLocation location = metadata->findVectorLocation(snapshot, 1);
	const PageLocation pageLocation = metadata->findPageLocation(snapshot, location.page_id);
	VectorPage page = vectors.read(snapshot, location.page_id);
	const VectorPageHeader header = page.header();
	std::vector<std::byte> record = page.read(location.slot_id, location.slot_generation);
	snapshot.release();
	LittleEndianCodec::writeU64(record, 0, 99);
	const PageOwnerHeader replacementOwner{header.page_id,
												 header.index_id,
												 PageOwnerKind::POK_VECTOR_RAW,
												 0,
												 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												 header.generation,
												 LogSequenceNumber(3)};
	VectorPage replacement = page.rebindOwner(replacementOwner);
	replacement.overwrite(location.slot_id, location.slot_generation, record);
	static_cast<void>(vectors.replace(replacement, pageLocation.segment_id, header.committed_lsn, LogSequenceNumber(3)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq(reclaimOptions(), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RecoversAnInterruptedDerivedPageRetirementBeforeRetrying) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		static_cast<void>(adjacencies.materialize(17, stagedRecords(), stagedOptions(), LogSequenceNumber(2)));
		DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
		MetadataPublishOptions interrupted;
		interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(3), interrupted)),
					 BootstrapPublishInterrupted);
	}

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *reopened);
	VectorAdjacencyPageStore adjacencies(io, *reopened);
	VectorManifestStore manifests(io, *reopened);
	SlottedPagePool slotted(io, *reopened);
	BTreePageStore trees(slotted, *reopened);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *reopened);
	const DerivedObjectReachabilityReclaimReport report = reclaimer.reclaim(reclaimOptions(), LogSequenceNumber(3));
	EXPECT_EQ(report.derived_adjacency_pages_retired, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RecoversAnInterruptedPqPageRetirementBeforeRetrying) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		const std::vector<PqVectorMaterializationRecord> records{{1, std::vector<uint8_t>(2000U, 1U)},
																						 {2, std::vector<uint8_t>(2000U, 2U)}};
		const VectorPqMaterializationOptions options{PhysicalSlotId(40), PhysicalSlotId(40), Generation(1), 16, 4};
		static_cast<void>(vectors.materializePq(17, records, options, LogSequenceNumber(2)));
		DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
		MetadataPublishOptions interrupted;
		interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaimPq(reclaimOptions(), LogSequenceNumber(3), interrupted)),
					 BootstrapPublishInterrupted);
	}

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *reopened);
	VectorAdjacencyPageStore adjacencies(io, *reopened);
	VectorManifestStore manifests(io, *reopened);
	SlottedPagePool slotted(io, *reopened);
	BTreePageStore trees(slotted, *reopened);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *reopened);
	const DerivedObjectReachabilityReclaimReport report = reclaimer.reclaimPq(reclaimOptions(), LogSequenceNumber(3));
	EXPECT_EQ(report.derived_pq_pages_retired, 2U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, EnforcesThePqRetirementBoundBeforePublishing) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::vector<PqVectorMaterializationRecord> records{{1, std::vector<uint8_t>(2000U, 1U)},
																						 {2, std::vector<uint8_t>(2000U, 2U)}};
	const VectorPqMaterializationOptions options{PhysicalSlotId(40), PhysicalSlotId(40), Generation(1), 16, 4};
	static_cast<void>(vectors.materializePq(17, records, options, LogSequenceNumber(2)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_retirements = 1;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimPq(limited, LogSequenceNumber(3))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest,
		ReclaimsStaleTypedMappingTreesAndPreservesPinnedSnapshotReads) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	static_cast<void>(VectorPqMapping::build(trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
												pqMappingOptions(STALE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
												LogSequenceNumber(2)));
	static_cast<void>(VectorAdjacencyMapping::build(
			trees, *metadata, STALE_ADJACENCY_MAPPING_INDEX_ID, adjacencyMappings(1U),
			mappingOptions(STALE_ADJACENCY_MAPPING_INDEX_ID, PhysicalSlotId(41)), LogSequenceNumber(3)));

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const IndexRootLocation pqRoot = metadata->findIndexRoot(pinned, STALE_PQ_MAPPING_INDEX_ID);
	const IndexRootLocation adjacencyRoot = metadata->findIndexRoot(pinned, STALE_ADJACENCY_MAPPING_INDEX_ID);
	EXPECT_EQ(VectorPqMapping::findPage(pinned, trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, 1), PageId(1001));
	EXPECT_EQ(VectorAdjacencyMapping::findPage(pinned, trees, *metadata, STALE_ADJACENCY_MAPPING_INDEX_ID, 1),
			  PageId(2001));

	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report =
			reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(4));
	EXPECT_EQ(report.scanned_generation, Generation(3));
	EXPECT_EQ(report.published_generation, Generation(4));
	EXPECT_EQ(report.mapping_roots_scanned, 2U);
	EXPECT_EQ(report.derived_mapping_roots_retired, 2U);
	EXPECT_EQ(report.derived_mapping_tree_pages_retired, 2U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(current, STALE_PQ_MAPPING_INDEX_ID).has_value());
	EXPECT_FALSE(metadata->findIndexRootOptional(current, STALE_ADJACENCY_MAPPING_INDEX_ID).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, pqRoot.root_page_id).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, adjacencyRoot.root_page_id).has_value());
	EXPECT_EQ(VectorPqMapping::findPage(pinned, trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, 1), PageId(1001));
	EXPECT_EQ(VectorAdjacencyMapping::findPage(pinned, trees, *metadata, STALE_ADJACENCY_MAPPING_INDEX_ID, 1),
			  PageId(2001));
	current.release();
	pinned.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RetainsTypedMappingTreesReferencedByVisibleManifests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	VectorManifest manifest;
	manifest.index_id = 17;
	manifest.dimensions = 2;
	manifest.metric = VectorDistanceMetric::VDM_L2;
	static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(2)));
	static_cast<void>(VectorPqMapping::build(trees, *metadata, VISIBLE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
												pqMappingOptions(VISIBLE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
												LogSequenceNumber(3)));
	MetadataSnapshotPin beforeBinding = metadata->openSnapshot();
	const IndexRootLocation visibleRoot = metadata->findIndexRoot(beforeBinding, VISIBLE_PQ_MAPPING_INDEX_ID);
	beforeBinding.release();
	bindPqMapping(*metadata, manifests, 17, visibleRoot, LogSequenceNumber(4));
	static_cast<void>(VectorAdjacencyMapping::build(
			trees, *metadata, STALE_ADJACENCY_MAPPING_INDEX_ID, adjacencyMappings(1U),
			mappingOptions(STALE_ADJACENCY_MAPPING_INDEX_ID, PhysicalSlotId(41)), LogSequenceNumber(5)));
	MetadataSnapshotPin beforeReclaim = metadata->openSnapshot();
	const IndexRootLocation staleRoot = metadata->findIndexRoot(beforeReclaim, STALE_ADJACENCY_MAPPING_INDEX_ID);
	beforeReclaim.release();

	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report =
			reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(6));
	EXPECT_EQ(report.reachable_mapping_roots, 1U);
	EXPECT_EQ(report.derived_mapping_roots_retired, 1U);
	EXPECT_EQ(report.derived_mapping_tree_pages_retired, 1U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, VISIBLE_PQ_MAPPING_INDEX_ID), visibleRoot);
	EXPECT_FALSE(metadata->findIndexRootOptional(current, STALE_ADJACENCY_MAPPING_INDEX_ID).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, staleRoot.root_page_id).has_value());
	EXPECT_EQ(VectorPqMapping::findPage(current, trees, *metadata, VISIBLE_PQ_MAPPING_INDEX_ID, 1), PageId(1001));
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, EnforcesMappingManifestBoundsBeforeScanningRoots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	for (const uint64_t indexId: {UINT64_C(17), UINT64_C(18)}) {
		VectorManifest manifest;
		manifest.index_id = indexId;
		manifest.dimensions = 2;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		static_cast<void>(manifests.materialize(
				manifest, PhysicalSlotId(45U + indexId - 17U), PhysicalSlotId(45U + indexId - 17U),
				LogSequenceNumber(indexId - 15U)));
	}
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_vector_manifests = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimMappingTrees(limited, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RejectsDuplicateVisibleMappingRoots) {
	// Two Manifests must not silently share a metadata-owned derived root: only
	// one logical Vector index may make a staged generation query-visible.
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
		for (const uint64_t indexId: {UINT64_C(17), UINT64_C(18)}) {
			VectorManifest manifest;
			manifest.index_id = indexId;
			manifest.dimensions = 2;
			manifest.metric = VectorDistanceMetric::VDM_L2;
			static_cast<void>(manifests.materialize(
					manifest, PhysicalSlotId(45U + indexId - 17U), PhysicalSlotId(45U + indexId - 17U),
					LogSequenceNumber(indexId - 15U)));
		}
		static_cast<void>(VectorPqMapping::build(trees, *metadata, VISIBLE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
													pqMappingOptions(VISIBLE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
													LogSequenceNumber(4)));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, VISIBLE_PQ_MAPPING_INDEX_ID);
		snapshot.release();
		bindPqMapping(*metadata, manifests, 17, root, LogSequenceNumber(5));
		bindPqMapping(*metadata, manifests, 18, root, LogSequenceNumber(6));
		DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
		expectFormatError(
				[&] { static_cast<void>(reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(7))); },
				FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, VISIBLE_PQ_MAPPING_INDEX_ID), root);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RetiresAMappingTreeFromASharedIndexPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	VectorPqMappingBuildOptions options = pqMappingOptions(STALE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40));
	options.tree.pool_owner_id = 99;
	static_cast<void>(VectorPqMapping::build(trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, pqMappings(1U), options,
												LogSequenceNumber(2)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report =
			reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(3));
	EXPECT_EQ(report.derived_mapping_roots_retired, 1U);
	EXPECT_GT(report.derived_mapping_tree_pages_retired, 0U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, STALE_PQ_MAPPING_INDEX_ID).has_value());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RetainsSystemCatalogAndUnknownFutureHighBitRoots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::array roots = {RSS_SYSTEM_CATALOG_INDEX_ID, FUTURE_HIGH_BIT_INDEX_ID};
	for (size_t index = 0; index < roots.size(); ++index) {
		buildNonDerivedRoot(trees, *metadata, roots[index], PhysicalSlotId(40U + index),
							LogSequenceNumber(2U + index));
	}
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	const DerivedObjectReachabilityReclaimReport report =
			reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(6));
	EXPECT_EQ(report.mapping_roots_scanned, roots.size());
	EXPECT_EQ(report.non_derived_mapping_roots_retained, roots.size());
	EXPECT_EQ(report.derived_mapping_roots_retired, 0U);
	EXPECT_EQ(report.published_generation, report.scanned_generation);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const uint64_t root: roots) {
		EXPECT_TRUE(metadata->findIndexRootOptional(snapshot, root).has_value());
	}
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, EnforcesMappingRootPageAndRetirementBoundsBeforePublishing) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	static_cast<void>(VectorPqMapping::build(trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
												pqMappingOptions(STALE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
												LogSequenceNumber(2)));
	static_cast<void>(VectorAdjacencyMapping::build(
			trees, *metadata, STALE_ADJACENCY_MAPPING_INDEX_ID, adjacencyMappings(1U),
			mappingOptions(STALE_ADJACENCY_MAPPING_INDEX_ID, PhysicalSlotId(41)), LogSequenceNumber(3)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	DerivedObjectReachabilityReclaimOptions limited = reclaimOptions();
	limited.maximum_mapping_roots = 1U;
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimMappingTrees(limited, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);

	const size_t mappingCount = pqMappingCountRequiringMultiplePages();
	const BulkBTreeBuildResult built = VectorPqMapping::build(
			trees, *metadata, LARGE_PQ_MAPPING_INDEX_ID, pqMappings(mappingCount),
			pqMappingOptions(LARGE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(50), mappingCount, 8U), LogSequenceNumber(4));
	ASSERT_GT(built.page_count, 1U);
	limited = reclaimOptions();
	limited.maximum_mapping_tree_pages = 1U;
	expectFormatError(
			[&] { static_cast<void>(reclaimer.reclaimMappingTrees(limited, LogSequenceNumber(5))); },
			FormatError::FERR_INVALID_STATE);
	limited = reclaimOptions();
	limited.maximum_retirements = 1U;
	expectFormatError(
			[&] { static_cast<void>(reclaimer.reclaimMappingTrees(limited, LogSequenceNumber(5))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findIndexRootOptional(current, LARGE_PQ_MAPPING_INDEX_ID).has_value());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, FailsClosedForMalformedVisibleMappingRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	VectorManifest manifest;
	manifest.index_id = 17;
	manifest.dimensions = 2;
	manifest.metric = VectorDistanceMetric::VDM_L2;
	static_cast<void>(manifests.materialize(manifest, PhysicalSlotId(45), PhysicalSlotId(45), LogSequenceNumber(2)));
	static_cast<void>(VectorPqMapping::build(trees, *metadata, VISIBLE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
												pqMappingOptions(VISIBLE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
												LogSequenceNumber(3)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, VISIBLE_PQ_MAPPING_INDEX_ID);
	VectorManifest corrupt = manifests.read(snapshot, 17).manifest();
	snapshot.release();
	corrupt.pq_mapping_index_id = root.index_id;
	corrupt.pq_mapping_root_page_id = root.root_page_id;
	corrupt.pq_mapping_root_generation = Generation(root.root_generation.value() + 1U);
	overwriteManifest(io, *metadata, corrupt);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, VISIBLE_PQ_MAPPING_INDEX_ID), root);
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, FailsClosedForMismatchedDerivedMappingPageOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
		static_cast<void>(VectorPqMapping::build(trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
													pqMappingOptions(STALE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
													LogSequenceNumber(2)));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, STALE_PQ_MAPPING_INDEX_ID);
		const PageLocation location = metadata->findPageLocation(snapshot, root.root_page_id);
		const BTreePage page = trees.read(snapshot, root.root_page_id);
		snapshot.release();
		const PageOwnerHeader foreignOwner{location.page_id,
									 STALE_ADJACENCY_MAPPING_INDEX_ID,
									 PageOwnerKind::POK_INDEX,
									 0,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 location.generation,
									 page.slottedPage().ownerCommittedLsn()};
		overwritePageOwner(io, *metadata, root.root_page_id, foreignOwner);
		DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
		expectFormatError(
				[&] { static_cast<void>(reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(3))); },
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_TRUE(metadata->findIndexRootOptional(current, STALE_PQ_MAPPING_INDEX_ID).has_value());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, FailsClosedForCyclicDerivedMappingTree) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	VectorAdjacencyPageStore adjacencies(io, *metadata);
	VectorManifestStore manifests(io, *metadata);
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const size_t mappingCount = pqMappingCountRequiringMultiplePages();
	const BulkBTreeBuildResult built = VectorPqMapping::build(
			trees, *metadata, LARGE_PQ_MAPPING_INDEX_ID, pqMappings(mappingCount),
			pqMappingOptions(LARGE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40), mappingCount, 8U), LogSequenceNumber(2));
	ASSERT_GT(built.root_level, 0U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, LARGE_PQ_MAPPING_INDEX_ID);
	const BTreePage original = trees.read(snapshot, root.root_page_id);
	const PageOwnerHeader owner{root.root_page_id,
								LARGE_PQ_MAPPING_INDEX_ID,
								PageOwnerKind::POK_INDEX,
								0,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								root.root_generation,
								original.slottedPage().ownerCommittedLsn()};
	BTreePageHeader cyclicHeader = original.header();
	cyclicHeader.leftmost_child = root.root_page_id;
	cyclicHeader.entry_count = 0U;
	BTreePage cyclic = BTreePage::create(owner, cyclicHeader);
	for (const BTreePageEntry &entry: original.entries()) {
		cyclic.insert(entry);
	}
	snapshot.release();
	static_cast<void>(trees.replace(cyclic, LogSequenceNumber(3)));
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
	expectFormatError([&] { static_cast<void>(reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(metadata->findIndexRootOptional(current, LARGE_PQ_MAPPING_INDEX_ID).has_value());
}

TEST_F(RelocatableDerivedObjectReachabilityReclaimerTest, RecoversInterruptedMappingTreeRetirementBeforeRetrying) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore vectors(io, *metadata);
		VectorAdjacencyPageStore adjacencies(io, *metadata);
		VectorManifestStore manifests(io, *metadata);
		SlottedPagePool slotted(io, *metadata);
		BTreePageStore trees(slotted, *metadata);
		static_cast<void>(VectorPqMapping::build(trees, *metadata, STALE_PQ_MAPPING_INDEX_ID, pqMappings(1U),
													pqMappingOptions(STALE_PQ_MAPPING_INDEX_ID, PhysicalSlotId(40)),
													LogSequenceNumber(2)));
		DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *metadata);
		MetadataPublishOptions interrupted;
		interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(3), interrupted)),
					 BootstrapPublishInterrupted);
	}

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore vectors(io, *reopened);
	VectorAdjacencyPageStore adjacencies(io, *reopened);
	VectorManifestStore manifests(io, *reopened);
	SlottedPagePool slotted(io, *reopened);
	BTreePageStore trees(slotted, *reopened);
	DerivedObjectReachabilityReclaimer reclaimer(trees, vectors, adjacencies, manifests, *reopened);
	const DerivedObjectReachabilityReclaimReport report =
			reclaimer.reclaimMappingTrees(reclaimOptions(), LogSequenceNumber(3));
	EXPECT_EQ(report.derived_mapping_roots_retired, 0U);
	EXPECT_EQ(report.derived_mapping_tree_pages_retired, 0U);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findIndexRootOptional(snapshot, STALE_PQ_MAPPING_INDEX_ID).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}
