/**
 * @file test_VectorRawReachabilityReclaimer.cpp
 * @brief Lifecycle and recovery tests for unreachable Raw Vector storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebook.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/maintenance/VectorIndexStateReclaimer.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/storage/relocatable/maintenance/VectorRawReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x5566778899AABBCCULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	RawVectorMaterializationRecord rawRecord(uint64_t vectorId) {
		RawVectorMaterializationRecord result;
		result.vector_id = vectorId;
		result.bytes.assign(16U, std::byte{0x7A});
		LittleEndianCodec::writeU64(result.bytes, 0U, vectorId);
		return result;
	}

} // namespace

class RelocatableVectorRawReachabilityReclaimerTest : public ::testing::Test {
protected:
	std::filesystem::path data_path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> data_file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> data_io_;
	std::shared_ptr<StorageIO> wal_io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<PhysicalSegmentAllocator> physical_;
	std::unique_ptr<SlottedPagePool> slotted_pages_;
	std::unique_ptr<BTreePageStore> trees_;
	std::unique_ptr<EntityPageStore> page_entities_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;
	std::unique_ptr<NativeGraphWritePlanner> native_writes_;
	std::unique_ptr<SystemCatalog> catalog_;
	std::unique_ptr<VectorPageStore> raw_pages_;
	std::unique_ptr<VectorAdjacencyPageStore> adjacency_pages_;
	std::unique_ptr<VectorManifestStore> manifests_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path_ = std::filesystem::temp_directory_path() / ("relocatable_raw_vector_reclaim_" + to_string(uuid));
		wal_path_ = data_path_.string() + ".foreign";
		data_file_ = std::make_shared<std::fstream>(data_path_,
													std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file_ = std::make_shared<std::fstream>(wal_path_,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file_->is_open());
		ASSERT_TRUE(wal_file_->is_open());
		data_io_ = std::make_shared<StorageIO>(data_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io_ = std::make_shared<StorageIO>(wal_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(data_io_, initialSuperblock());
		physical_ = std::make_unique<PhysicalSegmentAllocator>(data_io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(data_io_, *metadata_);
		trees_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		page_entities_ = std::make_unique<EntityPageStore>(data_io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(data_io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
		NativeGraphWritePlannerOptions writeOptions;
		writeOptions.first_slot = PhysicalSlotId(40U);
		writeOptions.last_slot = PhysicalSlotId(95U);
		writeOptions.entity_pool_owner_id = 9401U;
		writeOptions.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(95U), 9402U};
		native_writes_ = std::make_unique<NativeGraphWritePlanner>(*metadata_, *page_entities_, *labels_, *properties_,
																																										 *trees_, writeOptions);
		catalog_ = std::make_unique<SystemCatalog>(*trees_, *metadata_);
		raw_pages_ = std::make_unique<VectorPageStore>(data_io_, *metadata_);
		adjacency_pages_ = std::make_unique<VectorAdjacencyPageStore>(data_io_, *metadata_);
		manifests_ = std::make_unique<VectorManifestStore>(data_io_, *metadata_);
	}

	void TearDown() override {
		manifests_.reset();
		adjacency_pages_.reset();
		raw_pages_.reset();
		catalog_.reset();
		native_writes_.reset();
		properties_.reset();
		labels_.reset();
		page_entities_.reset();
		trees_.reset();
		slotted_pages_.reset();
		physical_.reset();
		metadata_.reset();
		data_io_.reset();
		wal_io_.reset();
		if (data_file_ && data_file_->is_open()) {
			data_file_->close();
		}
		if (wal_file_ && wal_file_->is_open()) {
			wal_file_->close();
		}
		std::error_code error;
		std::filesystem::remove(data_path_, error);
		std::filesystem::remove(wal_path_, error);
	}

	void reopenStores() {
		manifests_.reset();
		adjacency_pages_.reset();
		raw_pages_.reset();
		catalog_.reset();
		native_writes_.reset();
		properties_.reset();
		labels_.reset();
		page_entities_.reset();
		trees_.reset();
		slotted_pages_.reset();
		physical_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(data_io_);
		physical_ = std::make_unique<PhysicalSegmentAllocator>(data_io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(data_io_, *metadata_);
		trees_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		page_entities_ = std::make_unique<EntityPageStore>(data_io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(data_io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
		NativeGraphWritePlannerOptions writeOptions;
		writeOptions.first_slot = PhysicalSlotId(40U);
		writeOptions.last_slot = PhysicalSlotId(95U);
		writeOptions.entity_pool_owner_id = 9401U;
		writeOptions.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(95U), 9402U};
		native_writes_ = std::make_unique<NativeGraphWritePlanner>(*metadata_, *page_entities_, *labels_, *properties_,
																																										 *trees_, writeOptions);
		catalog_ = std::make_unique<SystemCatalog>(*trees_, *metadata_);
		raw_pages_ = std::make_unique<VectorPageStore>(data_io_, *metadata_);
		adjacency_pages_ = std::make_unique<VectorAdjacencyPageStore>(data_io_, *metadata_);
		manifests_ = std::make_unique<VectorManifestStore>(data_io_, *metadata_);
	}

	void overwriteManifestFrame(const VectorManifest &manifest) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const PageLocation location = metadata_->findPageLocation(snapshot, manifest.page_id);
		const CatalogEntry pool = metadata_->findCatalog(snapshot, location.segment_id);
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
		const auto pageBytes = page.serialize();
		data_io_->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		data_io_->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, pageBytes.data(), pageBytes.size());
	}

	// Rewrites one current Raw frame with a self-consistent but semantically
	// wrong owner. This exercises the reclaimer's independent directory-to-page
	// validation, rather than relying only on the page codec's checks.
	void overwriteRawFrameOwner(PageId pageId, uint64_t replacementIndexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const PageLocation location = metadata_->findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata_->findCatalog(snapshot, location.segment_id);
		const VectorPageHeader previous = raw_pages_->read(snapshot, pageId).header();
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();

		VectorPageHeader replacement = previous;
		replacement.index_id = replacementIndexId;
		replacement.live_count = 0U;
		const PageOwnerHeader owner{replacement.page_id,
									replacement.index_id,
									PageOwnerKind::POK_VECTOR_RAW,
									0U,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									replacement.generation,
									replacement.committed_lsn};
		const VectorPage page = VectorPage::create(owner, replacement);
		const auto ownerBytes = PageOwnerHeaderCodec::encode(owner);
		const std::vector<std::byte> pageBytes = page.serialize();
		data_io_->writeAt(offset, ownerBytes.data(), ownerBytes.size());
		data_io_->writeAt(offset + RSS_PAGE_OWNER_HEADER_BYTES, pageBytes.data(), pageBytes.size());
	}

	// The envelope remains valid while the subtype header names a different
	// logical Codebook. This models a stale or incorrectly rebuilt extent.
	void overwriteCodebookHeader(uint64_t codebookId, uint64_t replacementCodebookId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const ExtentLocation extent =
				metadata_->findExtentLocation(snapshot, VectorCodebookExtentIdCodec::manifestExtentId(codebookId));
		const CatalogEntry pool = metadata_->findCatalog(snapshot, extent.segment_id);
		const PhysicalSegmentContents contents = physical_->read(snapshot, extent.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot);
		snapshot.release();

		VectorCodebookExtentHeaderCodec::Bytes bytes{};
		std::copy(contents.header_extension.begin(), contents.header_extension.end(), bytes.begin());
		LittleEndianCodec::writeU64(bytes, 16U, replacementCodebookId);
		LittleEndianCodec::writeU32(bytes, 48U, 0U);
		LittleEndianCodec::writeU32(bytes, 48U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		data_io_->writeAt(offset + RSS_ENVELOPE_BYTES, bytes.data(), bytes.size());
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn() const {
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	[[nodiscard]] IndexDefinition vectorDefinition() const {
		return {0U,
				"person_embedding",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_VECTOR,
				7U,
				{"embedding"},
				2U,
				"l2"};
	}

	void publishIndex(IndexDefinition &definition) {
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		definition = session.createIndexDefinition(std::move(definition));
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	[[nodiscard]] IndexDefinition alternateVectorDefinition() const {
		IndexDefinition definition = vectorDefinition();
		definition.name = "company_embedding";
		definition.label_token_id = 8U;
		return definition;
	}

	void buildMappingRoot(uint64_t indexId) {
		const auto entries = std::make_shared<const std::vector<BTreePageEntry>>(std::vector<BTreePageEntry>{
				{VectorSemanticCodec::nodeMappingKey(1U), 1U}, {VectorSemanticCodec::nodeMappingKey(2U), 2U}});
		const OrderedBTreeEntrySource source = [entries] {
			auto position = std::make_shared<size_t>(0U);
			return [entries, position]() -> std::optional<BTreePageEntry> {
				if (*position == entries->size()) {
					return std::nullopt;
				}
				return (*entries)[(*position)++];
			};
		};
		const BulkBTreeBuildOptions options = {PhysicalSlotId(40U), PhysicalSlotId(95U), indexId, 2U, 1U};
		static_cast<void>(BulkBTreeBuilder::build(*trees_, *metadata_, indexId, source, options, nextCheckpointLsn()));
	}

	[[nodiscard]] IndexRootLocation buildVectorNodeMappingRoot(uint64_t indexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const uint64_t mappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata_, indexId);
		snapshot.release();
		const auto entries = std::make_shared<const std::vector<BTreePageEntry>>(
				std::vector<BTreePageEntry>{{VectorNodeMapping::key(1U), 1U}, {VectorNodeMapping::key(2U), 2U}});
		const OrderedBTreeEntrySource source = [entries] {
			auto position = std::make_shared<size_t>(0U);
			return [entries, position]() -> std::optional<BTreePageEntry> {
				if (*position == entries->size()) {
					return std::nullopt;
				}
				return (*entries)[(*position)++];
			};
		};
		VectorNodeMappingBuildOptions options;
		options.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, entries->size(), 1U};
		options.maximum_entries = entries->size();
		static_cast<void>(
				VectorNodeMapping::build(*trees_, *metadata_, mappingIndexId, source, options, nextCheckpointLsn()));
		snapshot = metadata_->openSnapshot();
		const IndexRootLocation root = metadata_->findIndexRoot(snapshot, mappingIndexId);
		snapshot.release();
		return root;
	}

	[[nodiscard]] VectorRawMaterializationResult materializeRaw(uint64_t indexId) {
		const std::array<RawVectorMaterializationRecord, 2U> records = {rawRecord(1U), rawRecord(2U)};
		VectorRawMaterializationOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(95U);
		options.maximum_records = records.size();
		options.maximum_pages = 2U;
		return raw_pages_->materializeRaw(indexId, records, options, nextCheckpointLsn());
	}

	[[nodiscard]] VectorRawMaterializationResult materializeTwoLargeRawPages(uint64_t indexId) {
		std::array<RawVectorMaterializationRecord, 2U> records = {rawRecord(1U), rawRecord(2U)};
		for (RawVectorMaterializationRecord &record: records) {
			record.bytes.assign(3000U, std::byte{0x5C});
			LittleEndianCodec::writeU64(record.bytes, 0U, record.vector_id);
		}
		VectorRawMaterializationOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(95U);
		options.maximum_records = records.size();
		options.maximum_pages = records.size();
		return raw_pages_->materializeRaw(indexId, records, options, nextCheckpointLsn());
	}

	void materializeCodebook(uint64_t codebookId, SegmentId segmentId = SegmentId(9701U)) {
		VectorCodebookStore codebooks(*physical_, *metadata_);
		VectorCodebookWriteRequest request;
		request.codebook_id = codebookId;
		request.manifest_segment_id = segmentId;
		request.generation = Generation(1U);
		request.element_type = VectorCodebookElementType::VCET_FLOAT32;
		request.dimensions = 2U;
		request.subspace_count = 1U;
		request.centroid_count = 3U;
		const std::vector<std::byte> payload(24U, std::byte{0x3A});
		static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(40U), PhysicalSlotId(95U),
												   nextCheckpointLsn()));
	}

	[[nodiscard]] VectorManifest materializeManifest(uint64_t indexId, uint64_t codebookId = 0U) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const IndexRootLocation root = metadata_->findIndexRoot(snapshot, indexId);
		snapshot.release();
		const IndexRootLocation vectorNodeRoot = buildVectorNodeMappingRoot(indexId);
		VectorManifest manifest;
		manifest.index_id = indexId;
		manifest.dimensions = 2U;
		manifest.metric = VectorDistanceMetric::VDM_L2;
		manifest.codebook_id = codebookId;
		if (codebookId != 0U) {
			manifest.codebook_generation = Generation(1U);
		}
		manifest.node_mapping_root_page_id = root.root_page_id;
		manifest.node_mapping_root_generation = root.root_generation;
		manifest.vector_node_mapping_index_id = vectorNodeRoot.index_id;
		manifest.vector_node_mapping_root_page_id = vectorNodeRoot.root_page_id;
		manifest.vector_node_mapping_root_generation = vectorNodeRoot.root_generation;
		return manifests_->materialize(manifest, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn())
				.manifest;
	}

	void dropIndex(uint64_t indexId) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<IndexDefinition> definition = catalog_->findIndexDefinitionById(snapshot, indexId);
		snapshot.release();
		ASSERT_TRUE(definition.has_value());
		NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
		SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
		session.dropIndexDefinition(*definition);
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	void dropIndex(const IndexDefinition &definition) { dropIndex(definition.index_id); }

	[[nodiscard]] VectorRawReachabilityReclaimOptions options() const { return {8U, 8U, 8U, 16U, 32U, 16U}; }

	[[nodiscard]] VectorIndexStateReclaimOptions stateOptions() const {
		VectorIndexStateReclaimOptions result;
		result.manifests = {8U, 8U};
		result.raw = options();
		result.derived = {8U, 32U, 32U, 16U, 16U, 16U, 16U};
		result.mapping_trees = {8U, 8U, 8U, 16U, 16U, 16U};
		result.codebooks = {8U, 16U, 64U, 64U, 16U};
		result.empty_pools = {8U, 64U, 256U, 64U, 32U};
		return result;
	}

	[[nodiscard]] VectorCodebookReachabilityReclaimOptions codebookOptions() const {
		return {8U, 16U, 64U, 64U, 16U};
	}

	[[nodiscard]] VectorRawReachabilityReclaimer reclaimer() const {
		return {*raw_pages_, *manifests_, *catalog_, *metadata_};
	}
};

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresDroppedRawPagesThenReleasesTheirPoolAfterPinnedSnapshotsDrain) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	const VectorRawReachabilityReclaimReport retained = reclaim.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retained.stale_manifest_raw_pools_retained, 1U);
	EXPECT_EQ(retained.raw_pages_retired, 0U);

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const PageLocation rawLocation = metadata_->findPageLocation(oldSnapshot, raw.pages.front().page_id);
	const CatalogEntry rawPool = metadata_->findCatalog(oldSnapshot, rawLocation.segment_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));
	const VectorRawReachabilityReclaimReport retired = reclaim.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retired.raw_pages_retired, 1U);
	EXPECT_EQ(retired.vector_locations_erased, 2U);
	EXPECT_EQ(retired.raw_pools_retired, 1U);
	EXPECT_EQ(retired.raw_pools_released, 0U);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findPageLocationOptional(current, raw.pages.front().page_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(current, 1U).has_value());
	EXPECT_EQ(metadata_->findCatalog(current, rawPool.segment_id).state, SegmentState::SGS_RETIRED);
	current.release();
	EXPECT_EQ(metadata_->findVectorLocation(oldSnapshot, 1U).page_id, raw.pages.front().page_id);
	EXPECT_EQ(raw_pages_->read(oldSnapshot, raw.pages.front().page_id).header().index_id, definition.index_id);
	oldSnapshot.release();

	const VectorRawReachabilityReclaimReport released = reclaim.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(released.raw_pools_released, 1U);
	MetadataSnapshotPin finalSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(finalSnapshot, rawPool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(finalSnapshot, rawPool.physical_slot));
	finalSnapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresMultipleDroppedRawPoolsInOneBoundedRun) {
	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	static_cast<void>(materializeRaw(first.index_id));
	const std::array<RawVectorMaterializationRecord, 2U> secondRecords = {rawRecord(3U), rawRecord(4U)};
	VectorRawMaterializationOptions rawOptions;
	rawOptions.first_slot = PhysicalSlotId(40U);
	rawOptions.last_slot = PhysicalSlotId(95U);
	rawOptions.maximum_records = secondRecords.size();
	rawOptions.maximum_pages = 2U;
	static_cast<void>(raw_pages_->materializeRaw(second.index_id, secondRecords, rawOptions, nextCheckpointLsn()));
	const VectorManifest firstManifest = materializeManifest(first.index_id);
	const VectorManifest secondManifest = materializeManifest(second.index_id);
	dropIndex(first.index_id);
	dropIndex(second.index_id);
	static_cast<void>(manifests_->retire(first.index_id, firstManifest.committed_lsn, nextCheckpointLsn()));
	static_cast<void>(manifests_->retire(second.index_id, secondManifest.committed_lsn, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	const VectorRawReachabilityReclaimReport report = reclaim.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.raw_pages_retired, 2U);
	EXPECT_EQ(report.raw_pools_retired, 2U);
	EXPECT_EQ(report.raw_pools_released, 2U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, ReleasesARawPoolWhosePageRetirementAlreadyCommitted) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageLocation location = metadata_->findPageLocation(snapshot, raw.pages.front().page_id);
	const CatalogEntry pool = metadata_->findCatalog(snapshot, location.segment_id);
	const VectorPageHeader header = raw_pages_->read(snapshot, raw.pages.front().page_id).header();
	const Generation generation = snapshot.superblock().generation;
	snapshot.release();
	const std::array<RetiredRawVectorPage, 1U> retiredPages = {{{location, definition.index_id, header.committed_lsn}}};
	const std::array<uint64_t, 2U> erasedVectorIds = {1U, 2U};
	static_cast<void>(raw_pages_->retireRawPages(generation, retiredPages, erasedVectorIds, nextCheckpointLsn()));

	const VectorRawReachabilityReclaimReport report = reclaimer().reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.raw_pages_scanned, 0U);
	EXPECT_EQ(report.raw_pages_retired, 0U);
	EXPECT_EQ(report.raw_pools_retired, 1U);
	EXPECT_EQ(report.raw_pools_released, 1U);

	MetadataSnapshotPin finalSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(finalSnapshot, pool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(finalSnapshot, pool.physical_slot));
	finalSnapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedWhenRawRetirementExhaustsTheCheckpointLsn) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), LogSequenceNumber(std::numeric_limits<uint64_t>::max()))),
				 FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAnActiveRawPoolWithoutItsRequiredManifest) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAnActiveRawPoolWhoseManifestMappingRootIsMissing) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	const std::array<uint64_t, 1U> erasedRoots = {definition.index_id};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAnActiveRawPoolOwnedByANonVectorIndex) {
	IndexDefinition scalar = {0U,
									"person_score",
									IndexDefinitionEntityKind::IDEK_NODE,
									IndexDefinitionKind::IDK_PROPERTY,
									7U,
									{"score"},
									0U,
									{}};
	publishIndex(scalar);
	buildMappingRoot(scalar.index_id);
	static_cast<void>(materializeRaw(scalar.index_id));
	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAnActiveManifestOwnedByANonVectorIndexDuringRawReclaim) {
	IndexDefinition scalar = {0U,
									"person_score",
									IndexDefinitionEntityKind::IDEK_NODE,
									IndexDefinitionKind::IDK_PROPERTY,
									7U,
									{"score"},
									0U,
									{}};
	publishIndex(scalar);
	buildMappingRoot(scalar.index_id);
	static_cast<void>(materializeManifest(scalar.index_id));

	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresDroppedNonVectorManifestAsOrphanedVectorState) {
	IndexDefinition scalar = {0U,
									"person_score",
									IndexDefinitionEntityKind::IDEK_NODE,
									IndexDefinitionKind::IDK_PROPERTY,
									7U,
									{"score"},
									0U,
									{}};
	publishIndex(scalar);
	buildMappingRoot(scalar.index_id);
	static_cast<void>(materializeManifest(scalar.index_id));
	dropIndex(scalar.index_id);

	const VectorRawReachabilityReclaimReport raw = reclaimer().reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(raw.vector_manifests, 1U);
	EXPECT_EQ(raw.raw_pages_retired, 0U);
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 1U);
	EXPECT_EQ(report.node_mappings.dropped_index_roots_retired, 2U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, scalar.index_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, scalar.index_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, AllowsDroppedOnlineManifestBeforeTheManifestPhaseRuns) {
	IndexDefinition definition = alternateVectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	dropIndex(definition);

	const VectorRawReachabilityReclaimReport report = reclaimer().reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.index_definitions, 0U);
	EXPECT_EQ(report.vector_manifests, 1U);
	EXPECT_EQ(report.raw_pools_retired, 0U);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestsOutsideEveryPublicIndexNamespaceDuringRawReclaim) {
	buildMappingRoot(7U);
	static_cast<void>(materializeManifest(7U));
	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAManifestWhoseFrameIdentityDiffersFromItsDirectoryKey) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	VectorManifest manifest = materializeManifest(definition.index_id);
	manifest.index_id = definition.index_id + 1U;
	overwriteManifestFrame(manifest);

	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsACurrentRawPoolWithAReservedOwnerIdentity) {
	const CatalogSlotReservationRequest request{SegmentId(9802U), SegmentKind::SGK_VECTOR_RAW_POOL, 0U,
												Generation(1U),	  SegmentState::SGS_SEALED,			true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsACurrentRawPoolWithAHighBitOwnerIdentity) {
	const CatalogSlotReservationRequest request{SegmentId(9807U), SegmentKind::SGK_VECTOR_RAW_POOL, UINT64_C(1) << 63U,
												Generation(1U),	  SegmentState::SGS_SEALED,			true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsACurrentRawPoolOutsideEveryPublicIndexNamespace) {
	const CatalogSlotReservationRequest request{SegmentId(9803U), SegmentKind::SGK_VECTOR_RAW_POOL, 7U,
												Generation(1U),	  SegmentState::SGS_SEALED,			true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsVectorLocationsWhoseRecordsNameAnotherVector) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	VectorLocation mismatched = metadata_->findVectorLocation(snapshot, 1U);
	const VectorLocation other = metadata_->findVectorLocation(snapshot, 2U);
	snapshot.release();
	mismatched.slot_id = other.slot_id;
	mismatched.slot_generation = other.slot_generation;
	static_cast<void>(metadata_->upsertVectorLocations(std::span(&mismatched, 1U), {}, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsRawPagesWhoseLiveSlotsOutnumberVectorLocations) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	const std::array<uint64_t, 1U> erased = {2U};
	static_cast<void>(metadata_->upsertVectorLocations({}, erased, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsARawPageWhoseOwnedIndexDisagreesWithItsPool) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	overwriteRawFrameOwner(raw.pages.front().page_id, definition.index_id + 1U);

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsVectorLocationScansThatExceedTheConfiguredBound) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	VectorRawReachabilityReclaimOptions bounded = options();
	bounded.maximum_vector_locations = 1U;

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsRawPageScansThatExceedTheConfiguredBound) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeTwoLargeRawPages(definition.index_id);
	ASSERT_EQ(raw.pages.size(), 2U);
	static_cast<void>(materializeManifest(definition.index_id));
	VectorRawReachabilityReclaimOptions bounded = options();
	bounded.maximum_raw_pages = 1U;

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsRawRetirementPlansThatExceedTheConfiguredBound) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeTwoLargeRawPages(definition.index_id);
	ASSERT_EQ(raw.pages.size(), 2U);
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));
	VectorRawReachabilityReclaimOptions bounded = options();
	bounded.maximum_retirements = 1U;

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsRawPoolAndManifestScansThatExceedTheirBounds) {
	static_cast<void>(materializeRaw(7U));
	const std::array<RawVectorMaterializationRecord, 2U> secondRecords = {rawRecord(3U), rawRecord(4U)};
	VectorRawMaterializationOptions rawOptions;
	rawOptions.first_slot = PhysicalSlotId(40U);
	rawOptions.last_slot = PhysicalSlotId(95U);
	rawOptions.maximum_records = secondRecords.size();
	rawOptions.maximum_pages = 2U;
	static_cast<void>(raw_pages_->materializeRaw(8U, secondRecords, rawOptions, nextCheckpointLsn()));
	VectorRawReachabilityReclaimOptions poolBound = options();
	poolBound.maximum_raw_pools = 1U;
	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(poolBound, nextCheckpointLsn())), FormatException);

	reopenStores();

	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	static_cast<void>(materializeManifest(first.index_id));
	static_cast<void>(materializeManifest(second.index_id));
	VectorRawReachabilityReclaimOptions manifestBound = options();
	manifestBound.maximum_vector_manifests = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(manifestBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsRetiredRawPoolsThatExceedTheRemainingPoolBound) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));

	MetadataSnapshotPin pinned = metadata_->openSnapshot();
	const VectorRawReachabilityReclaimReport retired = reclaimer().reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(retired.raw_pools_retired, 1U);
	EXPECT_EQ(retired.raw_pools_released, 0U);
	const CatalogSlotReservationRequest current{
			SegmentId(9811U), SegmentKind::SGK_VECTOR_RAW_POOL, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST,
			Generation(1U),	  SegmentState::SGS_SEALED,			true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(current, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	VectorRawReachabilityReclaimOptions bounded = options();
	bounded.maximum_raw_pools = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(bounded, nextCheckpointLsn())), FormatException);
	pinned.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsUnboundedLifecycleMaintenanceOptionsBeforeScanning) {
	VectorRawReachabilityReclaimer raw = reclaimer();
	VectorRawReachabilityReclaimOptions rawOptions = options();
	rawOptions.maximum_raw_pages = 0U;
	EXPECT_THROW(static_cast<void>(raw.reclaim(rawOptions, nextCheckpointLsn())), FormatException);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	VectorIndexStateReclaimOptions stateOptions = this->stateOptions();
	stateOptions.manifests.maximum_vector_manifests = 0U;
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions, nextCheckpointLsn())), FormatException);

	VectorCodebookReachabilityReclaimer codebooks(*physical_, *manifests_, *metadata_);
	VectorCodebookReachabilityReclaimOptions codebookOptions = this->codebookOptions();
	codebookOptions.maximum_extent_locations = 0U;
	EXPECT_THROW(static_cast<void>(codebooks.reclaim(codebookOptions, nextCheckpointLsn())), FormatException);

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	VectorPagePoolReclaimOptions poolOptions = this->stateOptions().empty_pools;
	poolOptions.maximum_page_locations = 0U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(poolOptions, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsEveryRawMaintenanceBoundBeforeScanning) {
	const std::array bounds = {&VectorRawReachabilityReclaimOptions::maximum_index_definitions,
							   &VectorRawReachabilityReclaimOptions::maximum_vector_manifests,
							   &VectorRawReachabilityReclaimOptions::maximum_raw_pools,
							   &VectorRawReachabilityReclaimOptions::maximum_raw_pages,
							   &VectorRawReachabilityReclaimOptions::maximum_vector_locations,
							   &VectorRawReachabilityReclaimOptions::maximum_retirements};
	VectorRawReachabilityReclaimer reclaim = reclaimer();
	for (const auto bound: bounds) {
		VectorRawReachabilityReclaimOptions invalid = options();
		invalid.*bound = 0U;
		EXPECT_THROW(static_cast<void>(reclaim.reclaim(invalid, nextCheckpointLsn())), FormatException);
		invalid = options();
		invalid.*bound = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(reclaim.reclaim(invalid, nextCheckpointLsn())), FormatException);
	}
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), {})), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsEveryCodebookMaintenanceBoundBeforeScanning) {
	const std::array bounds = {&VectorCodebookReachabilityReclaimOptions::maximum_vector_manifests,
							   &VectorCodebookReachabilityReclaimOptions::maximum_extent_locations,
							   &VectorCodebookReachabilityReclaimOptions::maximum_catalog_entries,
							   &VectorCodebookReachabilityReclaimOptions::maximum_retire_queue_entries,
							   &VectorCodebookReachabilityReclaimOptions::maximum_retirements};
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	for (const auto bound: bounds) {
		VectorCodebookReachabilityReclaimOptions invalid = codebookOptions();
		invalid.*bound = 0U;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
		invalid = codebookOptions();
		invalid.*bound = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
	}
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), {})), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsEveryTypedPoolMaintenanceBoundBeforeScanning) {
	const std::array bounds = {&VectorPagePoolReclaimOptions::maximum_index_definitions,
							   &VectorPagePoolReclaimOptions::maximum_catalog_entries,
							   &VectorPagePoolReclaimOptions::maximum_page_locations,
							   &VectorPagePoolReclaimOptions::maximum_retire_queue_entries,
							   &VectorPagePoolReclaimOptions::maximum_pools};
	VectorPagePoolReclaimer reclaimer(*slotted_pages_, *catalog_, *metadata_);
	for (const auto bound: bounds) {
		VectorPagePoolReclaimOptions invalid = stateOptions().empty_pools;
		invalid.*bound = 0U;
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
		invalid = stateOptions().empty_pools;
		invalid.*bound = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
	}
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(stateOptions().empty_pools, {})), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsEveryNestedLifecycleBoundBeforeScanning) {
	VectorIndexStateReclaimer reclaimer(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_,
										*manifests_, *catalog_, *metadata_);
	const auto expectInvalid = [&](const auto &invalidate) {
		VectorIndexStateReclaimOptions invalid = stateOptions();
		invalidate(invalid);
		EXPECT_THROW(static_cast<void>(reclaimer.reclaim(invalid, nextCheckpointLsn())), FormatException);
	};

	expectInvalid([](auto &options) { options.manifests.maximum_index_definitions = 0U; });
	expectInvalid([](auto &options) { options.manifests.maximum_vector_manifests = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_index_definitions = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_vector_manifests = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_raw_pools = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_raw_pages = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_vector_locations = 0U; });
	expectInvalid([](auto &options) { options.raw.maximum_retirements = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_vector_manifests = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_vector_locations = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_mapping_entries = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_candidate_pages = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_retirements = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_mapping_roots = 0U; });
	expectInvalid([](auto &options) { options.derived.maximum_mapping_tree_pages = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_index_definitions = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_constraint_definitions = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_vector_manifests = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_index_roots = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_tree_pages = 0U; });
	expectInvalid([](auto &options) { options.mapping_trees.maximum_retirements = 0U; });
	expectInvalid([](auto &options) { options.codebooks.maximum_vector_manifests = 0U; });
	expectInvalid([](auto &options) { options.codebooks.maximum_extent_locations = 0U; });
	expectInvalid([](auto &options) { options.codebooks.maximum_catalog_entries = 0U; });
	expectInvalid([](auto &options) { options.codebooks.maximum_retirements = 0U; });
	expectInvalid([](auto &options) { options.empty_pools.maximum_index_definitions = 0U; });
	expectInvalid([](auto &options) { options.empty_pools.maximum_catalog_entries = 0U; });
	expectInvalid([](auto &options) { options.empty_pools.maximum_page_locations = 0U; });
	expectInvalid([](auto &options) { options.empty_pools.maximum_pools = 0U; });
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(stateOptions(), {})), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestAndCatalogScansThatExceedTheirConfiguredBounds) {
	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	static_cast<void>(materializeManifest(first.index_id));
	static_cast<void>(materializeManifest(second.index_id));

	VectorCodebookReachabilityReclaimer codebooks(*physical_, *manifests_, *metadata_);
	VectorCodebookReachabilityReclaimOptions codebookOptions = this->codebookOptions();
	codebookOptions.maximum_vector_manifests = 1U;
	EXPECT_THROW(static_cast<void>(codebooks.reclaim(codebookOptions, nextCheckpointLsn())), FormatException);

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	VectorPagePoolReclaimOptions poolOptions = stateOptions().empty_pools;
	poolOptions.maximum_catalog_entries = 1U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(poolOptions, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsPageDirectoryAndRetiredPoolScansThatExceedTheirBounds) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	VectorPagePoolReclaimOptions pageBound = stateOptions().empty_pools;
	pageBound.maximum_page_locations = 1U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(pageBound, nextCheckpointLsn())), FormatException);

	dropIndex(definition.index_id);
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport retired = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_GE(retired.empty_pools.empty_pools_retired, 2U);
	VectorPagePoolReclaimOptions retiredBound = stateOptions().empty_pools;
	retiredBound.maximum_pools = 1U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(retiredBound, nextCheckpointLsn())), FormatException);
	oldSnapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsTypedVectorPoolsWithoutAVectorSchemaOwner) {
	IndexDefinition scalar = {0U,
									"person_score",
									IndexDefinitionEntityKind::IDEK_NODE,
									IndexDefinitionKind::IDK_PROPERTY,
									7U,
									{"score"},
									0U,
									{}};
	publishIndex(scalar);
	buildMappingRoot(scalar.index_id);
	static_cast<void>(materializeManifest(scalar.index_id));
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsTypedVectorPoolsOutsideEveryPublicIndexNamespace) {
	buildMappingRoot(7U);
	static_cast<void>(materializeManifest(7U));
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsATypedVectorPoolWithAReservedOwnerIdentity) {
	const CatalogSlotReservationRequest request{SegmentId(9801U),
												SegmentKind::SGK_VECTOR_MANIFEST_POOL,
												0U,
												Generation(1U),
												SegmentState::SGS_SEALED,
												true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsATypedVectorPoolWithAHighBitOwnerIdentity) {
	const CatalogSlotReservationRequest request{SegmentId(9808U),		  SegmentKind::SGK_VECTOR_MANIFEST_POOL,
												UINT64_C(1) << 63U,		  Generation(1U),
												SegmentState::SGS_SEALED, true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsATypedVectorPoolWhoseCatalogSlotWasReleased) {
	const CatalogSlotReservationRequest request{SegmentId(9804U),
												SegmentKind::SGK_VECTOR_MANIFEST_POOL,
												7U,
												Generation(1U),
												SegmentState::SGS_SEALED,
												true};
	const std::optional<CatalogSlotReservation> reservation = metadata_->reserveFirstFreeAndUpsertCatalog(
			request, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn());
	ASSERT_TRUE(reservation.has_value());
	static_cast<void>(
			metadata_->setPhysicalSlotAllocated(reservation->entry.physical_slot, false, nextCheckpointLsn()));

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, IgnoresAnAllocatedObsoleteTypedPool) {
	const CatalogSlotReservationRequest request{SegmentId(9805U),
												SegmentKind::SGK_VECTOR_MANIFEST_POOL,
												7U,
												Generation(1U),
												SegmentState::SGS_OBSOLETE,
												true};
	ASSERT_TRUE(metadata_
						->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(95U),
														   nextCheckpointLsn())
						.has_value());
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	const VectorPagePoolReclaimReport report = pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn());
	EXPECT_EQ(report.catalog_entries_scanned, 2U);
	EXPECT_EQ(report.empty_pools_retired, 0U);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsCallerSuppliedCanonicalDefinitionId) {
	IndexDefinition invalid = vectorDefinition();
	invalid.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
	NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
	SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(invalid)), FormatException);
	transaction.rollback();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsDroppingAnAbsentCanonicalDefinition) {
	IndexDefinition absent = vectorDefinition();
	absent.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
	NativeGraphWriteTransaction transaction = native_writes_->beginTransaction();
	SystemCatalogWriteSession session = catalog_->beginWrite(transaction);
	EXPECT_THROW(session.dropIndexDefinition(absent), FormatException);
	transaction.rollback();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetainsATypedPoolWithAPendingRelocationSource) {
	const CatalogSlotReservationRequest request{SegmentId(9806U),
												SegmentKind::SGK_VECTOR_MANIFEST_POOL,
													RSS_SYSTEM_CATALOG_INDEX_ID_FIRST,
												Generation(1U),
												SegmentState::SGS_SEALED,
												true};
	const std::optional<CatalogSlotReservation> reservation = metadata_->reserveFirstFreeAndUpsertCatalog(
			request, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn());
	ASSERT_TRUE(reservation.has_value());
	const CatalogSlotRelocationPreparer prepare = [](const CatalogEntry &, const CatalogEntry &) {};
	ASSERT_TRUE(metadata_
						->relocateCatalogSegment(reservation->entry.segment_id, reservation->entry.generation,
												 PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn(), prepare)
						.has_value());

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	const VectorPagePoolReclaimReport report = pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn());
	EXPECT_EQ(report.empty_pools_retired, 0U);
	EXPECT_EQ(report.retired_pools_released, 0U);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsInvalidNestedOptionsBeforePublishingAnyLifecyclePhase) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	dropIndex(definition.index_id);

	MetadataSnapshotPin beforeSnapshot = metadata_->openSnapshot();
	const Generation beforeGeneration = beforeSnapshot.superblock().generation;
	beforeSnapshot.release();
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	VectorIndexStateReclaimOptions invalid = stateOptions();
	invalid.raw.maximum_raw_pages = 0U;
	EXPECT_THROW(static_cast<void>(state.reclaim(invalid, nextCheckpointLsn())), FormatException);

	MetadataSnapshotPin afterSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(afterSnapshot.superblock().generation, beforeGeneration);
	EXPECT_TRUE(metadata_->findVectorManifestOptional(afterSnapshot, definition.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(afterSnapshot, 1U).has_value());
	EXPECT_TRUE(metadata_->findPageLocationOptional(afterSnapshot, raw.pages.front().page_id).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(afterSnapshot, definition.index_id).has_value());
	afterSnapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestScansThatExceedTheConfiguredStateBound) {
	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	static_cast<void>(materializeManifest(first.index_id));
	static_cast<void>(materializeManifest(second.index_id));
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	VectorIndexStateReclaimOptions bounded = stateOptions();
	bounded.manifests.maximum_vector_manifests = 1U;
	EXPECT_THROW(static_cast<void>(state.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestsOutsideThePublicIndexNamespaces) {
	buildMappingRoot(7U);
	static_cast<void>(materializeManifest(7U));
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, LeavesAnEmptyMetadataGenerationUnchanged) {
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const Generation before = snapshot.superblock().generation;
	snapshot.release();
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.published_generation, before);
	EXPECT_EQ(report.raw.published_generation, before);
	EXPECT_EQ(report.adjacency.published_generation, before);
	EXPECT_EQ(report.pq.published_generation, before);
	EXPECT_EQ(report.derived_mappings.published_generation, before);
	EXPECT_EQ(report.node_mappings.published_generation, before);
	EXPECT_EQ(report.codebooks.published_generation, before);
	EXPECT_EQ(report.empty_pools.published_generation, before);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresMultipleDroppedManifestsWithSequentialCheckpointLsns) {
	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	static_cast<void>(materializeManifest(first.index_id));
	static_cast<void>(materializeManifest(second.index_id));
	dropIndex(first.index_id);
	dropIndex(second.index_id);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, first.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, second.index_id).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresDroppedVectorStateInDependencyOrder) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	materializeCodebook(17U);
	const VectorManifest manifest = materializeManifest(definition.index_id, 17U);
	dropIndex(definition.index_id);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 1U);
	EXPECT_EQ(report.raw.raw_pages_retired, 1U);
	EXPECT_EQ(report.raw.raw_pools_released, 1U);
	EXPECT_EQ(report.node_mappings.dropped_index_roots_retired, 2U);
	EXPECT_EQ(report.node_mappings.index_tree_pages_retired, 2U);
	EXPECT_EQ(report.codebooks.codebooks_retired, 1U);
	EXPECT_EQ(report.codebooks.codebook_extents_released, 1U);
	EXPECT_GE(report.empty_pools.empty_pools_retired, 2U);
	EXPECT_GE(report.empty_pools.retired_pools_released, 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, definition.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(snapshot, 1U).has_value());
	EXPECT_FALSE(metadata_->findPageLocationOptional(snapshot, raw.pages.front().page_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
	static_cast<void>(manifest);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, ReleasesEveryDroppedVectorPoolOnlyAfterPinnedSnapshotsDrain) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport retired = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(retired.raw.raw_pools_released, 0U);
	EXPECT_GE(retired.empty_pools.empty_pools_retired, 2U);
	EXPECT_EQ(retired.empty_pools.retired_pools_released, 0U);
	EXPECT_EQ(manifests_->read(oldSnapshot, definition.index_id).manifest(), manifest);
	oldSnapshot.release();

	const VectorIndexStateReclaimReport released = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(released.raw.raw_pools_released, 1U);
	EXPECT_GE(released.empty_pools.retired_pools_released, 2U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetainsActiveVectorStateAcrossEveryRetirementPhase) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	materializeCodebook(17U);
	static_cast<void>(materializeManifest(definition.index_id, 17U));

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.active_manifests_retained, 1U);
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 0U);
	EXPECT_EQ(report.raw.active_raw_pools_retained, 1U);
	EXPECT_EQ(report.raw.raw_pages_retired, 0U);
	EXPECT_EQ(report.node_mappings.visible_vector_roots_retained, 2U);
	EXPECT_EQ(report.codebooks.reachable_codebooks, 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findVectorManifestOptional(snapshot, definition.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(snapshot, 1U).has_value());
	EXPECT_TRUE(metadata_->findPageLocationOptional(snapshot, raw.pages.front().page_id).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetainsActiveOnlineVectorStateAcrossEveryRetirementPhase) {
	IndexDefinition definition = alternateVectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.index_definitions, 1U);
	EXPECT_EQ(report.manifests.active_manifests_retained, 1U);
	EXPECT_EQ(report.raw.index_definitions, 1U);
	EXPECT_EQ(report.raw.active_raw_pools_retained, 1U);
	EXPECT_EQ(report.node_mappings.visible_vector_roots_retained, 2U);
	EXPECT_EQ(report.empty_pools.index_definitions, 1U);
	EXPECT_EQ(report.empty_pools.nonempty_pools_retained, 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findVectorManifestOptional(snapshot, definition.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(snapshot, 1U).has_value());
	EXPECT_TRUE(metadata_->findPageLocationOptional(snapshot, raw.pages.front().page_id).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetainsAnActiveOnlineVectorRawPool) {
	IndexDefinition definition = alternateVectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	const VectorRawReachabilityReclaimReport report = reclaim.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.index_definitions, 1U);
	EXPECT_EQ(report.active_raw_pools_retained, 1U);
	EXPECT_EQ(report.raw_pages_retired, 0U);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest,
		RejectsAnActiveManifestWhoseDeltaMappingRootWasReplaced) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	VectorManifest manifest = materializeManifest(definition.index_id);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const uint64_t deltaMappingIndexId = VectorDeltaMapping::selectMappingIndexId(snapshot, *metadata_, definition.index_id);
	snapshot.release();
	const std::array<VectorDeltaEntry, 1U> entries = {{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
	const VectorDeltaMappingBuildOptions buildOptions{
			{PhysicalSlotId(40U), PhysicalSlotId(95U), deltaMappingIndexId, entries.size(), 1U}, entries.size()};
	static_cast<void>(VectorDeltaMapping::build(*trees_, *metadata_, deltaMappingIndexId, entries, buildOptions,
												 nextCheckpointLsn()));
	snapshot = metadata_->openSnapshot();
	const IndexRootLocation firstDeltaRoot = metadata_->findIndexRoot(snapshot, deltaMappingIndexId);
	snapshot.release();
	manifest.delta_mapping_index_id = deltaMappingIndexId;
	manifest.delta_mapping_root_page_id = firstDeltaRoot.root_page_id;
	manifest.delta_mapping_root_generation = firstDeltaRoot.root_generation;
	manifest.delta_entry_count = entries.size();
	overwriteManifestFrame(manifest);

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	EXPECT_NO_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())));

	const std::array<uint64_t, 1U> erasedRoots = {deltaMappingIndexId};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));
	static_cast<void>(VectorDeltaMapping::build(*trees_, *metadata_, deltaMappingIndexId, entries, buildOptions,
												 nextCheckpointLsn()));
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedForAnActiveManifestWhoseMappingRootIsMissing) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	const std::array<uint64_t, 1U> erasedRoots = {definition.index_id};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findVectorManifestOptional(snapshot, definition.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(snapshot, 1U).has_value());
	EXPECT_TRUE(metadata_->findPageLocationOptional(snapshot, raw.pages.front().page_id).has_value());
	snapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedForAnActiveManifestWhoseMappingRootWasReplaced) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	const std::array<uint64_t, 1U> erasedRoots = {definition.index_id};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));
	buildMappingRoot(definition.index_id);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest,
		FailsClosedForAnActiveManifestWhoseVectorNodeMappingRootWasReplaced) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorManifest manifest = materializeManifest(definition.index_id);
	const std::array<uint64_t, 1U> erasedRoots = {manifest.vector_node_mapping_index_id};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));
	static_cast<void>(buildVectorNodeMappingRoot(definition.index_id));

	EXPECT_THROW(static_cast<void>(reclaimer().reclaim(options(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest,
		FailsClosedForAnActiveManifestWhoseDeltaMappingRootWasReplaced) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	VectorManifest manifest = materializeManifest(definition.index_id);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const uint64_t deltaMappingIndexId =
			VectorDeltaMapping::selectMappingIndexId(snapshot, *metadata_, definition.index_id);
	snapshot.release();
	const std::array<VectorDeltaEntry, 1U> entries = {{{1U, 1U, VectorDeltaOperation::VDO_INSERT}}};
	const VectorDeltaMappingBuildOptions buildOptions{
			{PhysicalSlotId(40U), PhysicalSlotId(95U), deltaMappingIndexId, entries.size(), 1U}, entries.size()};
	static_cast<void>(
			VectorDeltaMapping::build(*trees_, *metadata_, deltaMappingIndexId, entries, buildOptions, nextCheckpointLsn()));
	snapshot = metadata_->openSnapshot();
	const IndexRootLocation originalDeltaRoot = metadata_->findIndexRoot(snapshot, deltaMappingIndexId);
	snapshot.release();
	manifest.delta_mapping_index_id = deltaMappingIndexId;
	manifest.delta_mapping_root_page_id = originalDeltaRoot.root_page_id;
	manifest.delta_mapping_root_generation = originalDeltaRoot.root_generation;
	manifest.delta_entry_count = entries.size();
	overwriteManifestFrame(manifest);
	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
																								*catalog_, *metadata_);
	EXPECT_EQ(state.reclaim(stateOptions(), nextCheckpointLsn()).manifests.active_manifests_retained, 1U);

	const std::array<uint64_t, 1U> erasedRoots = {deltaMappingIndexId};
	static_cast<void>(trees_->publishRoots({}, erasedRoots, nextCheckpointLsn()));
	static_cast<void>(
			VectorDeltaMapping::build(*trees_, *metadata_, deltaMappingIndexId, entries, buildOptions, nextCheckpointLsn()));
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation beforeGeneration = before.superblock().generation;
	before.release();

	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, beforeGeneration);
	EXPECT_TRUE(metadata_->findVectorManifestOptional(after, definition.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(after, 1U).has_value());
	EXPECT_TRUE(metadata_->findPageLocationOptional(after, raw.pages.front().page_id).has_value());
	after.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsAManifestOwnedByAnActiveNonVectorIndex) {
	IndexDefinition definition = {0U,
										"person_score",
										IndexDefinitionEntityKind::IDEK_NODE,
										IndexDefinitionKind::IDK_PROPERTY,
										7U,
										{"score"},
										0U,
										{}};
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresDroppedOnlineVectorStateWithoutASealedTombstone) {
	IndexDefinition definition = alternateVectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	const VectorRawMaterializationResult raw = materializeRaw(definition.index_id);
	static_cast<void>(materializeManifest(definition.index_id));
	dropIndex(definition);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = state.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 1U);
	EXPECT_EQ(report.raw.raw_pages_retired, 1U);
	EXPECT_EQ(report.raw.raw_pools_released, 1U);
	EXPECT_EQ(report.node_mappings.dropped_index_roots_retired, 2U);
	EXPECT_EQ(report.node_mappings.index_tree_pages_retired, 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, definition.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(snapshot, 1U).has_value());
	EXPECT_FALSE(metadata_->findPageLocationOptional(snapshot, raw.pages.front().page_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value());
	snapshot.release();
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, ReleasesAnEmptyDerivedMappingPoolAfterItsRootIsRetired) {
	const uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(7);
	const std::array<VectorPqPageMapping, 1U> mappings = {{{1U, PageId(1U)}}};
	VectorPqMappingBuildOptions buildOptions;
	buildOptions.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, mappings.size(), 1U};
	buildOptions.maximum_entries = mappings.size();
	static_cast<void>(
			VectorPqMapping::build(*trees_, *metadata_, mappingIndexId, mappings, buildOptions, nextCheckpointLsn()));

	DerivedObjectReachabilityReclaimer derived(*trees_, *raw_pages_, *adjacency_pages_, *manifests_, *metadata_);
	const DerivedObjectReachabilityReclaimReport mappingReport =
			derived.reclaimMappingTrees(stateOptions().derived, nextCheckpointLsn());
	EXPECT_EQ(mappingReport.derived_mapping_roots_retired, 1U);
	EXPECT_EQ(mappingReport.derived_mapping_tree_pages_retired, 1U);

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	const VectorPagePoolReclaimReport poolReport = pools.reclaim(stateOptions().empty_pools, nextCheckpointLsn());
	EXPECT_EQ(poolReport.empty_pools_retired, 1U);
	EXPECT_EQ(poolReport.retired_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedWhenTypedPoolRetirementExhaustsTheCheckpointLsn) {
	const uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(7);
	const std::array<VectorPqPageMapping, 1U> mappings = {{{1U, PageId(1U)}}};
	VectorPqMappingBuildOptions buildOptions;
	buildOptions.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, mappings.size(), 1U};
	buildOptions.maximum_entries = mappings.size();
	static_cast<void>(
			VectorPqMapping::build(*trees_, *metadata_, mappingIndexId, mappings, buildOptions, nextCheckpointLsn()));

	DerivedObjectReachabilityReclaimer derived(*trees_, *raw_pages_, *adjacency_pages_, *manifests_, *metadata_);
	EXPECT_EQ(
			derived.reclaimMappingTrees(stateOptions().derived, nextCheckpointLsn()).derived_mapping_tree_pages_retired,
			1U);

	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	EXPECT_THROW(static_cast<void>(pools.reclaim(stateOptions().empty_pools,
												 LogSequenceNumber(std::numeric_limits<uint64_t>::max()))),
				 FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsTwoEmptyTypedPoolsWhenTheBoundAllowsOnlyOne) {
	const auto buildMapping = [&](uint64_t mappingIndexId, PageId pageId) {
		const std::array<VectorPqPageMapping, 1U> mappings = {{{1U, pageId}}};
		VectorPqMappingBuildOptions buildOptions;
		buildOptions.tree = {PhysicalSlotId(40U), PhysicalSlotId(95U), mappingIndexId, mappings.size(), 1U};
		buildOptions.maximum_entries = mappings.size();
		static_cast<void>(VectorPqMapping::build(*trees_, *metadata_, mappingIndexId, mappings, buildOptions,
												 nextCheckpointLsn()));
	};
	buildMapping(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(7), PageId(1U));
	buildMapping(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(8), PageId(2U));

	DerivedObjectReachabilityReclaimer derived(*trees_, *raw_pages_, *adjacency_pages_, *manifests_, *metadata_);
	EXPECT_EQ(
			derived.reclaimMappingTrees(stateOptions().derived, nextCheckpointLsn()).derived_mapping_tree_pages_retired,
			2U);
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	VectorPagePoolReclaimOptions bounded = stateOptions().empty_pools;
	bounded.maximum_pools = 1U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RecoversInterruptedManifestPhaseBeforeRetryingTheWholeLifecycle) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	dropIndex(definition.index_id);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(state.reclaim(stateOptions(), nextCheckpointLsn(), interrupted)),
				 BootstrapPublishInterrupted);

	reopenStores();
	VectorIndexStateReclaimer retry(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	const VectorIndexStateReclaimReport report = retry.reclaim(stateOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.manifests.dropped_manifests_retired, 1U);
	EXPECT_EQ(report.raw.raw_pages_retired, 1U);
	EXPECT_EQ(report.node_mappings.dropped_index_roots_retired, 2U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedWhenManifestRetirementExhaustsTheCheckpointLsn) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	static_cast<void>(materializeManifest(definition.index_id));
	dropIndex(definition.index_id);

	VectorIndexStateReclaimer state(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_, *manifests_,
									*catalog_, *metadata_);
	EXPECT_THROW(
			static_cast<void>(state.reclaim(stateOptions(), LogSequenceNumber(std::numeric_limits<uint64_t>::max()))),
			FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RecoversInterruptedCodebookRetirementBeforeRetrying) {
	materializeCodebook(17U);
	VectorCodebookReachabilityReclaimer codebooks(*physical_, *manifests_, *metadata_);
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(codebooks.reclaim(codebookOptions(), nextCheckpointLsn(), interrupted)),
				 BootstrapPublishInterrupted);

	reopenStores();

	VectorCodebookReachabilityReclaimer retry(*physical_, *manifests_, *metadata_);
	const VectorCodebookReachabilityReclaimReport report = retry.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.codebooks_retired, 1U);
	EXPECT_EQ(report.codebook_extents_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresEveryExtentOfOneUnreferencedLargeCodebook) {
	VectorCodebookWriteRequest request;
	request.codebook_id = 17U;
	request.manifest_segment_id = SegmentId(9701U);
	request.generation = Generation(1U);
	request.element_type = VectorCodebookElementType::VCET_FLOAT32;
	request.dimensions = 768U;
	request.subspace_count = 96U;
	request.centroid_count = 256U;
	const size_t payloadBytes = static_cast<size_t>(request.dimensions) * request.centroid_count * sizeof(float);
	const size_t extentCount = VectorCodebookStore::requiredDataExtentCount(payloadBytes);
	for (size_t ordinal = 0U; ordinal < extentCount; ++ordinal) {
		const SegmentId segmentId(9702U + ordinal);
		request.data_extents.push_back({VectorCodebookExtentIdCodec::dataExtentId(segmentId), segmentId});
	}
	const std::vector<std::byte> payload(payloadBytes, std::byte{0x3A});
	VectorCodebookStore codebooks(*physical_, *metadata_);
	static_cast<void>(
			codebooks.writeImmutable(request, payload, PhysicalSlotId(40U), PhysicalSlotId(95U), nextCheckpointLsn()));

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	const VectorCodebookReachabilityReclaimReport report = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.codebooks_retired, 1U);
	EXPECT_EQ(report.codebook_extents_retired, extentCount + 1U);
	EXPECT_EQ(report.codebook_extents_released, extentCount + 1U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_
						 ->findExtentLocationOptional(
								 snapshot, VectorCodebookExtentIdCodec::manifestExtentId(request.codebook_id))
						 .has_value());
	for (const VectorCodebookDataExtent &extent: request.data_extents) {
		EXPECT_FALSE(metadata_->findExtentLocationOptional(snapshot, extent.extent_id).has_value());
	}
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetainsRetiredCodebookForPinnedSnapshotsUntilRelease) {
	materializeCodebook(17U);
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	VectorCodebookStore codebooks(*physical_, *metadata_);
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const ExtentLocation extent =
			metadata_->findExtentLocation(oldSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U));
	const CatalogEntry pool = metadata_->findCatalog(oldSnapshot, extent.segment_id);
	const VectorCodebook oldCodebook = codebooks.read(oldSnapshot, 17U);
	EXPECT_EQ(oldCodebook.header.codebook_id, 17U);

	const VectorCodebookReachabilityReclaimReport retired = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(retired.codebooks_retired, 1U);
	EXPECT_EQ(retired.codebook_extents_released, 0U);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findExtentLocationOptional(current, VectorCodebookExtentIdCodec::manifestExtentId(17U))
						 .has_value());
	EXPECT_EQ(metadata_->findCatalog(current, pool.segment_id).state, SegmentState::SGS_RETIRED);
	current.release();
	EXPECT_EQ(codebooks.read(oldSnapshot, 17U).header, oldCodebook.header);
	oldSnapshot.release();

	const VectorCodebookReachabilityReclaimReport released = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(released.codebook_extents_released, 1U);
	MetadataSnapshotPin finalSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(finalSnapshot, pool.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(finalSnapshot, pool.physical_slot));
	finalSnapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, FailsClosedWhenCodebookRetirementExhaustsTheCheckpointLsn) {
	materializeCodebook(17U);
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(
						 reclaimer.reclaim(codebookOptions(), LogSequenceNumber(std::numeric_limits<uint64_t>::max()))),
				 FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsARetiredCodebookWhoseCatalogSlotWasReleased) {
	materializeCodebook(17U);
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const ExtentLocation extent =
			metadata_->findExtentLocation(oldSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U));
	const CatalogEntry codebook = metadata_->findCatalog(oldSnapshot, extent.segment_id);
	EXPECT_EQ(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn()).codebooks_retired, 1U);
	static_cast<void>(metadata_->setPhysicalSlotAllocated(codebook.physical_slot, false, nextCheckpointLsn()));
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn())), FormatException);
	oldSnapshot.release();
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestReferencesToADeletedCodebookExtent) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	materializeCodebook(17U);
	static_cast<void>(materializeManifest(definition.index_id));
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_EQ(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn()).codebooks_retired, 1U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	VectorManifest manifest = manifests_->read(snapshot, definition.index_id).manifest();
	snapshot.release();
	manifest.codebook_id = 17U;
	manifest.codebook_generation = Generation(1U);
	overwriteManifestFrame(manifest);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsManifestReferencesToTheWrongCodebookGeneration) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	materializeCodebook(17U);
	static_cast<void>(materializeManifest(definition.index_id, 17U));
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	VectorManifest manifest = manifests_->read(snapshot, definition.index_id).manifest();
	snapshot.release();
	manifest.codebook_generation = Generation(2U);
	overwriteManifestFrame(manifest);

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsACodebookExtentWhoseHeaderDisagreesWithItsCatalogOwner) {
	materializeCodebook(17U);
	overwriteCodebookHeader(17U, 18U);

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsConflictingManifestGenerationsForOneCodebook) {
	IndexDefinition first = vectorDefinition();
	IndexDefinition second = alternateVectorDefinition();
	second.name = "company_embedding";
	second.label_token_id = 8U;
	publishIndex(first);
	publishIndex(second);
	buildMappingRoot(first.index_id);
	buildMappingRoot(second.index_id);
	materializeCodebook(17U);
	static_cast<void>(materializeManifest(first.index_id));
	static_cast<void>(materializeManifest(second.index_id));
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	VectorManifest firstManifest = manifests_->read(snapshot, first.index_id).manifest();
	VectorManifest secondManifest = manifests_->read(snapshot, second.index_id).manifest();
	snapshot.release();
	firstManifest.codebook_id = 17U;
	firstManifest.codebook_generation = Generation(1U);
	secondManifest.codebook_id = 17U;
	secondManifest.codebook_generation = Generation(1U);
	overwriteManifestFrame(firstManifest);
	overwriteManifestFrame(secondManifest);

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	const VectorCodebookReachabilityReclaimReport retained = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(retained.reachable_codebooks, 1U);
	EXPECT_EQ(retained.reachable_codebook_extents, 1U);

	secondManifest.codebook_generation = Generation(2U);
	overwriteManifestFrame(secondManifest);
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(codebookOptions(), nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsCodebookExtentScansThatExceedTheConfiguredBound) {
	materializeCodebook(17U, SegmentId(9701U));
	materializeCodebook(18U, SegmentId(9702U));
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	VectorCodebookReachabilityReclaimOptions bounded = codebookOptions();
	bounded.maximum_extent_locations = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(bounded, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsCodebookRetirementAndCatalogScansThatExceedTheirBounds) {
	materializeCodebook(17U, SegmentId(9701U));
	materializeCodebook(18U, SegmentId(9702U));
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	VectorCodebookReachabilityReclaimOptions retirementBound = codebookOptions();
	retirementBound.maximum_retirements = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(retirementBound, nextCheckpointLsn())), FormatException);
	VectorCodebookReachabilityReclaimOptions catalogBound = codebookOptions();
	catalogBound.maximum_catalog_entries = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(catalogBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsMaintenanceRetireQueueScansThatExceedTheConfiguredBound) {
	materializeCodebook(17U, SegmentId(9701U));
	materializeCodebook(18U, SegmentId(9702U));
	VectorCodebookStore codebooks(*physical_, *metadata_);
	ASSERT_TRUE(codebooks.relocateExtent(17U, VectorCodebookExtentIdCodec::manifestExtentId(17U), PhysicalSlotId(50U),
									 PhysicalSlotId(95U), nextCheckpointLsn())
									.has_value());
	ASSERT_TRUE(codebooks.relocateExtent(18U, VectorCodebookExtentIdCodec::manifestExtentId(18U), PhysicalSlotId(50U),
									 PhysicalSlotId(95U), nextCheckpointLsn())
									.has_value());
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	VectorCodebookReachabilityReclaimOptions bounded = codebookOptions();
	bounded.maximum_retire_queue_entries = 1U;
	EXPECT_THROW(static_cast<void>(reclaimer.reclaim(bounded, nextCheckpointLsn())), FormatException);
	VectorPagePoolReclaimer pools(*slotted_pages_, *catalog_, *metadata_);
	VectorPagePoolReclaimOptions poolBound = stateOptions().empty_pools;
	poolBound.maximum_retire_queue_entries = 1U;
	EXPECT_THROW(static_cast<void>(pools.reclaim(poolBound, nextCheckpointLsn())), FormatException);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresMultipleUnreferencedCodebooksInOneBoundedRun) {
	materializeCodebook(17U, SegmentId(9701U));
	materializeCodebook(18U, SegmentId(9702U));
	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	const VectorCodebookReachabilityReclaimReport report = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(report.codebooks_retired, 2U);
	EXPECT_EQ(report.codebook_extents_released, 2U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RejectsReclaimersComposedFromDifferentMetadataStores) {
	const std::filesystem::path foreignPath = data_path_.string() + ".foreign";
	const std::filesystem::path foreignWalPath = foreignPath.string() + ".foreign";
	auto foreignDataFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																			   std::ios::trunc);
	auto foreignWalFile = std::make_shared<std::fstream>(foreignWalPath, std::ios::in | std::ios::out |
																				 std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignDataFile->is_open());
	ASSERT_TRUE(foreignWalFile->is_open());
	{
		auto foreignDataIo = std::make_shared<StorageIO>(foreignDataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignWalIo = std::make_shared<StorageIO>(foreignWalFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignDataIo, initialSuperblock());
		PhysicalSegmentAllocator foreignPhysical(foreignDataIo, *foreignMetadata);
		SlottedPagePool foreignSlottedPages(foreignDataIo, *foreignMetadata);
		BTreePageStore foreignTrees(foreignSlottedPages, *foreignMetadata);
		SystemCatalog foreignCatalog(foreignTrees, *foreignMetadata);
		VectorPageStore foreignRawPages(foreignDataIo, *foreignMetadata);
		VectorAdjacencyPageStore foreignAdjacencyPages(foreignDataIo, *foreignMetadata);
		VectorManifestStore foreignManifests(foreignDataIo, *foreignMetadata);
		EXPECT_THROW(VectorRawReachabilityReclaimer(foreignRawPages, *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorRawReachabilityReclaimer(*raw_pages_, foreignManifests, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorRawReachabilityReclaimer(*raw_pages_, *manifests_, foreignCatalog, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(foreignPhysical, *slotted_pages_, *trees_, *raw_pages_,
											   *adjacency_pages_, *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, foreignSlottedPages, *trees_, *raw_pages_, *adjacency_pages_,
											   *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, *slotted_pages_, foreignTrees, *raw_pages_,
											   *adjacency_pages_, *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, *slotted_pages_, *trees_, foreignRawPages, *adjacency_pages_,
											   *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, *slotted_pages_, *trees_, *raw_pages_, foreignAdjacencyPages,
											   *manifests_, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_,
											   foreignManifests, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorIndexStateReclaimer(*physical_, *slotted_pages_, *trees_, *raw_pages_, *adjacency_pages_,
													   *manifests_, foreignCatalog, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorPagePoolReclaimer(foreignSlottedPages, *catalog_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorPagePoolReclaimer(*slotted_pages_, foreignCatalog, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorCodebookReachabilityReclaimer(foreignPhysical, *manifests_, *metadata_),
					 std::invalid_argument);
		EXPECT_THROW(VectorCodebookReachabilityReclaimer(*physical_, foreignManifests, *metadata_),
					 std::invalid_argument);
	}
	foreignDataFile->close();
	foreignWalFile->close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RetiresAnUnreferencedCodebookAfterPhysicalRelocation) {
	materializeCodebook(17U);
	VectorCodebookStore codebooks(*physical_, *metadata_);
	ASSERT_TRUE(codebooks
						.relocateExtent(17U, VectorCodebookExtentIdCodec::manifestExtentId(17U), PhysicalSlotId(41U),
										PhysicalSlotId(95U), nextCheckpointLsn())
						.has_value());
	MetadataSnapshotPin beforeReclaim = metadata_->openSnapshot();
	EXPECT_EQ(
			metadata_->findExtentLocation(beforeReclaim, VectorCodebookExtentIdCodec::manifestExtentId(17U)).generation,
			Generation(2U));
	EXPECT_EQ(codebooks.read(beforeReclaim, 17U).header.generation, Generation(1U));
	beforeReclaim.release();

	VectorCodebookReachabilityReclaimer reclaimer(*physical_, *manifests_, *metadata_);
	const VectorCodebookReachabilityReclaimReport retained = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(retained.relocated_codebooks_retained, 1U);
	EXPECT_EQ(retained.codebooks_retired, 0U);
	static_cast<void>(physical_->releaseRelocatedSource(SegmentId(9701U), Generation(2U), nextCheckpointLsn()));
	const VectorCodebookReachabilityReclaimReport retired = reclaimer.reclaim(codebookOptions(), nextCheckpointLsn());
	EXPECT_EQ(retired.codebooks_retired, 1U);
	EXPECT_EQ(retired.codebook_extents_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}

TEST_F(RelocatableVectorRawReachabilityReclaimerTest, RecoversInterruptedRawRetirementBeforeRetrying) {
	IndexDefinition definition = vectorDefinition();
	publishIndex(definition);
	buildMappingRoot(definition.index_id);
	static_cast<void>(materializeRaw(definition.index_id));
	const VectorManifest manifest = materializeManifest(definition.index_id);
	dropIndex(definition.index_id);
	static_cast<void>(manifests_->retire(definition.index_id, manifest.committed_lsn, nextCheckpointLsn()));

	VectorRawReachabilityReclaimer reclaim = reclaimer();
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(reclaim.reclaim(options(), nextCheckpointLsn(), interrupted)),
				 BootstrapPublishInterrupted);

	reopenStores();

	VectorRawReachabilityReclaimer retry = reclaimer();
	const VectorRawReachabilityReclaimReport report = retry.reclaim(options(), nextCheckpointLsn());
	EXPECT_EQ(report.raw_pages_retired, 1U);
	EXPECT_EQ(report.raw_pools_retired, 1U);
	EXPECT_EQ(report.raw_pools_released, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(data_io_, *metadata_).scan()));
}
