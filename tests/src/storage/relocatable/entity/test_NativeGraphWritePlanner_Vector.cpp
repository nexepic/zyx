/**
 * @file test_NativeGraphWritePlanner_Vector.cpp
 * @brief Native Vector bootstrap publication tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

using namespace graph::storage;
using graph::PropertyValue;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7A7B7C7D7E7F8082ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	IndexDefinition vectorDefinition() {
		IndexDefinition result;
		result.index_id = 77U;
		result.name = "native-vector";
		result.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		result.kind = IndexDefinitionKind::IDK_VECTOR;
		result.label_token_id = 7U;
		result.property_names = {"embedding"};
		result.vector_dimensions = 2U;
		result.vector_metric = "l2";
		return result;
	}

	NativeGraphVectorBootstrapOptions vectorOptions() {
		NativeGraphVectorBootstrapOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.maximum_vectors = 8U;
		options.maximum_pages = 16U;
		return options;
	}

	NativeGraphVectorDeltaOptions vectorDeltaOptions() {
		NativeGraphVectorDeltaOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.maximum_records = 8U;
		options.maximum_raw_pages = 8U;
		options.rebuild_threshold = 16U;
		options.maximum_overlay_entries = 64U;
		options.maximum_delta_pages = 8U;
		return options;
	}

	NativeGraphMutationExecutorOptions mutationExecutorOptions() {
		NativeGraphMutationExecutorOptions result;
		result.maximum_index_definitions = 32U;
		result.maximum_constraint_definitions = 32U;
		result.maximum_constraint_entity_scan = 1024U;
		result.maximum_constraint_support_collisions = 16U;
		result.maximum_vector_records = 32U;
		result.maximum_vector_pages = 16U;
		result.vector_delta_rebuild_threshold = 16U;
		result.maximum_vector_delta_overlay_entries = 64U;
		result.maximum_vector_delta_pages = 8U;
		return result;
	}

	OfflineVectorPqRebuildOptions pqRebuildOptions() {
		constexpr uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x99);
		OfflineVectorPqRebuildOptions options;
		options.training = {2U, 1U, 1U, 2U, 8U, 1U};
		options.codebook.codebook_id = 71U;
		options.codebook.manifest_segment_id = SegmentId(71U);
		options.codebook.generation = Generation(1U);
		options.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		options.codebook.dimensions = 2U;
		options.codebook.subspace_count = 1U;
		options.codebook.centroid_count = 1U;
		options.codebook_first_slot = PhysicalSlotId(50U);
		options.codebook_last_slot = PhysicalSlotId(50U);
		options.pages = {PhysicalSlotId(51U), PhysicalSlotId(51U), Generation(1U), 8U, 4U};
		options.mapping_index_id = mappingIndexId;
		options.mapping.tree = {PhysicalSlotId(52U), PhysicalSlotId(52U), mappingIndexId, 8U, 4U};
		options.mapping.maximum_entries = 8U;
		return options;
	}

} // namespace

class NativeGraphWritePlannerVectorTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> slotted_pages_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;
	std::unique_ptr<BTreePageStore> indexes_;
	std::unique_ptr<VectorPageStore> raw_pages_;
	std::unique_ptr<VectorManifestStore> manifests_;
	std::unique_ptr<PhysicalSegmentAllocator> physical_;
	std::unique_ptr<VectorCodebookStore> codebooks_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("native_graph_vector_bootstrap_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
		indexes_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		raw_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
		manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
		physical_ = std::make_unique<PhysicalSegmentAllocator>(io_, *metadata_);
		codebooks_ = std::make_unique<VectorCodebookStore>(*physical_, *metadata_);
	}

	void TearDown() override {
		codebooks_.reset();
		physical_.reset();
		manifests_.reset();
		raw_pages_.reset();
		indexes_.reset();
		properties_.reset();
		labels_.reset();
		slotted_pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	void reopenNativeStores() {
		codebooks_.reset();
		physical_.reset();
		manifests_.reset();
		raw_pages_.reset();
		indexes_.reset();
		properties_.reset();
		labels_.reset();
		slotted_pages_.reset();
		entities_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
		indexes_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		raw_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
		manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
		physical_ = std::make_unique<PhysicalSegmentAllocator>(io_, *metadata_);
		codebooks_ = std::make_unique<VectorCodebookStore>(*physical_, *metadata_);
	}
};

TEST_F(NativeGraphWritePlannerVectorTest, PublishesRawMappingsAndManifestInOneGeneration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(labels);
	const uint64_t secondNode = transaction.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 2U> records = {{
			{secondNode, {std::byte{0x20}, std::byte{0x40}, std::byte{0x60}, std::byte{0x80}}},
			{firstNode, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										  vectorOptions());
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());
	// The Node identity lease precedes the atomic Vector publication.
	EXPECT_EQ(committed.superblock.generation, Generation(3U));
	EXPECT_THROW(static_cast<void>(metadata_->findIndexRoot(before, vectorDefinition().index_id)), FormatException);
	EXPECT_FALSE(metadata_->findVectorManifestOptional(before, vectorDefinition().index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(before, 1U).has_value());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const VectorManifestPage manifestPage = manifests_->read(after, vectorDefinition().index_id);
	const VectorManifest &manifest = manifestPage.manifest();
	ASSERT_TRUE(manifest.node_mapping_root_page_id.isValid());
	ASSERT_TRUE(manifest.vector_node_mapping_root_page_id.isValid());
	ASSERT_TRUE(manifest.raw_write_tail_page_id.isValid());
	EXPECT_EQ(manifest.vector_node_mapping_index_id,
			  VectorNodeMapping::selectMappingIndexId(before, *metadata_, vectorDefinition().index_id));

	BTreeIndex nodeMapping(vectorDefinition().index_id, *indexes_, *metadata_);
	BTreeIndex vectorNodeMapping(manifest.vector_node_mapping_index_id, *indexes_, *metadata_);
	EXPECT_EQ(nodeMapping.find(after, VectorSemanticCodec::nodeMappingKey(firstNode)), std::optional<uint64_t>(1U));
	EXPECT_EQ(nodeMapping.find(after, VectorSemanticCodec::nodeMappingKey(secondNode)), std::optional<uint64_t>(2U));
	EXPECT_EQ(vectorNodeMapping.find(after, VectorNodeMapping::key(1U)), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(vectorNodeMapping.find(after, VectorNodeMapping::key(2U)), std::optional<uint64_t>(secondNode));

	const VectorLocation firstLocation = metadata_->findVectorLocation(after, 1U);
	const VectorLocation secondLocation = metadata_->findVectorLocation(after, 2U);
	EXPECT_EQ(firstLocation.page_id, secondLocation.page_id);
	const VectorPage raw = raw_pages_->read(after, firstLocation.page_id);
	EXPECT_EQ(LittleEndianCodec::readU64(raw.read(firstLocation.slot_id, firstLocation.slot_generation), 0U), 1U);
	EXPECT_EQ(LittleEndianCodec::readU64(raw.read(secondLocation.slot_id, secondLocation.slot_generation), 0U), 2U);
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsZeroNormCosineBootstrapPayload) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition cosine = vectorDefinition();
	cosine.vector_metric = "cosine";
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<uint64_t, 1U> labels = {7U};
	const uint64_t nodeId = transaction.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}},
	}};

	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, cosine, records,
													   vectorOptions()),
				 FormatException);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsBootstrapMappingsToMissingOrErasedNodesBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<NativeGraphVectorBootstrapRecord, 1U> missing = {{
			{91U, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), missing,
													   vectorOptions()),
				 FormatException);
	transaction.rollback();

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	ASSERT_TRUE(create.commit().committed());
	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseNode(nodeId);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> erased = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	EXPECT_THROW(erase.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), erased,
												 vectorOptions()),
				 FormatException);
	erase.rollback();

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, vectorDefinition().index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(snapshot, vectorDefinition().index_id).has_value());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsMalformedBootstrapContractsWithoutPublishingAnyVectorDirectoryState) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<NativeGraphVectorBootstrapRecord, 1U> placeholder = {{
			{91U, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	const std::array<NativeGraphVectorBootstrapRecord, 0U> emptyRecords{};
	const auto expectRejectedBeforeRecordLookup = [&](const IndexDefinition &definition,
													  std::span<const NativeGraphVectorBootstrapRecord> records,
													  NativeGraphVectorBootstrapOptions options) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, records,
														   options),
					 FormatException);
		transaction.rollback();
	};

	IndexDefinition nonVector = vectorDefinition();
	nonVector.kind = IndexDefinitionKind::IDK_PROPERTY;
	expectRejectedBeforeRecordLookup(nonVector, placeholder, vectorOptions());
	IndexDefinition nonNode = vectorDefinition();
	nonNode.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	expectRejectedBeforeRecordLookup(nonNode, placeholder, vectorOptions());
	IndexDefinition noIndexId = vectorDefinition();
	noIndexId.index_id = 0U;
	expectRejectedBeforeRecordLookup(noIndexId, placeholder, vectorOptions());
	expectRejectedBeforeRecordLookup(vectorDefinition(), emptyRecords, vectorOptions());
	const std::array<NativeGraphVectorBootstrapRecord, 2U> tooManyRecords = {{placeholder.front(), placeholder.front()}};
	NativeGraphVectorBootstrapOptions noVectors = vectorOptions();
	noVectors.maximum_vectors = 0U;
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, noVectors);
	NativeGraphVectorBootstrapOptions tooManyVectors = vectorOptions();
	tooManyVectors.maximum_vectors = 1U;
	expectRejectedBeforeRecordLookup(vectorDefinition(), tooManyRecords, tooManyVectors);
	NativeGraphVectorBootstrapOptions noPages = vectorOptions();
	noPages.maximum_pages = 0U;
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, noPages);
	NativeGraphVectorBootstrapOptions invalidFirstSlot = vectorOptions();
	invalidFirstSlot.first_slot = PhysicalSlotId{};
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, invalidFirstSlot);
	NativeGraphVectorBootstrapOptions invalidLastSlot = vectorOptions();
	invalidLastSlot.last_slot = PhysicalSlotId{};
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, invalidLastSlot);
	NativeGraphVectorBootstrapOptions reversedSlots = vectorOptions();
	reversedSlots.first_slot = PhysicalSlotId(81U);
	reversedSlots.last_slot = PhysicalSlotId(80U);
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, reversedSlots);
	IndexDefinition oversized = vectorDefinition();
	oversized.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS + 1U;
	expectRejectedBeforeRecordLookup(oversized, placeholder, vectorOptions());
	NativeGraphVectorBootstrapOptions exactlyOnePage = vectorOptions();
	exactlyOnePage.maximum_pages = 1U;
	expectRejectedBeforeRecordLookup(vectorDefinition(), placeholder, exactlyOnePage);
	NativeGraphVectorBootstrapRecord wrongWidth = placeholder.front();
	wrongWidth.encoded_bfloat16.pop_back();
	std::array<NativeGraphVectorBootstrapRecord, 1U> wrongWidthRecords = {{wrongWidth}};
	expectRejectedBeforeRecordLookup(vectorDefinition(), wrongWidthRecords, vectorOptions());
	NativeGraphVectorBootstrapRecord zeroNodeId = placeholder.front();
	zeroNodeId.node_id = 0U;
	std::array<NativeGraphVectorBootstrapRecord, 1U> zeroNodeIdRecords = {{zeroNodeId}};
	expectRejectedBeforeRecordLookup(vectorDefinition(), zeroNodeIdRecords, vectorOptions());

	NativeGraphWriteTransaction duplicate = planner.beginTransaction();
	const uint64_t nodeId = duplicate.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 2U> duplicateRecords = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
			{nodeId, {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	EXPECT_THROW(duplicate.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
													 duplicateRecords, vectorOptions()),
				 FormatException);
	duplicate.rollback();

	NativeGraphWriteTransaction bounded = planner.beginTransaction();
	const uint64_t boundedNodeId = bounded.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> boundedRecords = {{
			{boundedNodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	NativeGraphVectorBootstrapOptions tooFewPages = vectorOptions();
	tooFewPages.maximum_pages = 2U;
	EXPECT_THROW(bounded.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
												   boundedRecords, tooFewPages),
				 FormatException);
	bounded.rollback();

	MetadataSnapshotPin afterRejected = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(afterRejected, vectorDefinition().index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(afterRejected, vectorDefinition().index_id).has_value());
	afterRejected.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsVectorStoresFromAnotherMetadataDomainBeforePlanning) {
	const std::filesystem::path foreignPath = path_.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
															 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	auto foreignSlottedPages = std::make_unique<SlottedPagePool>(foreignIo, *foreignMetadata);
	auto foreignIndexes = std::make_unique<BTreePageStore>(*foreignSlottedPages, *foreignMetadata);
	auto foreignRawPages = std::make_unique<VectorPageStore>(foreignIo, *foreignMetadata);
	auto foreignManifests = std::make_unique<VectorManifestStore>(foreignIo, *foreignMetadata);

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};

	// Every store participates in the same atomic publication, so each must
	// share the transaction's metadata domain before any Vector plan is derived.
	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*foreignIndexes, *raw_pages_, *manifests_, vectorDefinition(),
																				 records, vectorOptions()),
			 FormatException);
	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *foreignRawPages, *manifests_, vectorDefinition(),
																				 records, vectorOptions()),
			 FormatException);
	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *foreignManifests, vectorDefinition(),
																				 records, vectorOptions()),
			 FormatException);
	const std::array<NativeVectorMutation, 1U> mutations = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	EXPECT_THROW(transaction.stageVectorIndexDelta(*foreignIndexes, *raw_pages_, *manifests_, vectorDefinition(),
																		mutations, vectorDeltaOptions()),
			 FormatException);
	EXPECT_THROW(transaction.stageVectorIndexDelta(*indexes_, *foreignRawPages, *manifests_, vectorDefinition(),
																		mutations, vectorDeltaOptions()),
			 FormatException);
	EXPECT_THROW(transaction.stageVectorIndexDelta(*indexes_, *raw_pages_, *foreignManifests, vectorDefinition(),
																		mutations, vectorDeltaOptions()),
			 FormatException);
	transaction.rollback();

	foreignManifests.reset();
	foreignRawPages.reset();
	foreignIndexes.reset();
	foreignSlottedPages.reset();
	foreignMetadata.reset();
	foreignIo.reset();
	foreignFile->close();
	foreignFile.reset();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(NativeGraphWritePlannerVectorTest, RefusesToOverwriteAPublishedVectorBootstrapState) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction secondBootstrap = planner.beginTransaction();
	EXPECT_THROW(secondBootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
														   records, vectorOptions()),
				 FormatException);
	secondBootstrap.rollback();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(current, vectorDefinition().index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorManifestOptional(current, vectorDefinition().index_id).has_value());
	current.release();
}

TEST_F(NativeGraphWritePlannerVectorTest,
	   RejectsMalformedDeltaContractsAndDuplicateSemanticTransitionsBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<NativeVectorMutation, 1U> placeholder = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 91U,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	const std::array<NativeVectorMutation, 0U> emptyMutations{};
	const auto expectRejectedBeforeRootLookup = [&](const IndexDefinition &definition,
													std::span<const NativeVectorMutation> mutations,
													NativeGraphVectorDeltaOptions options) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		EXPECT_THROW(
				transaction.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, mutations, options),
				FormatException);
		transaction.rollback();
	};

	IndexDefinition nonVector = vectorDefinition();
	nonVector.kind = IndexDefinitionKind::IDK_PROPERTY;
	expectRejectedBeforeRootLookup(nonVector, placeholder, vectorDeltaOptions());
	IndexDefinition nonNode = vectorDefinition();
	nonNode.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	expectRejectedBeforeRootLookup(nonNode, placeholder, vectorDeltaOptions());
	IndexDefinition noIndexId = vectorDefinition();
	noIndexId.index_id = 0U;
	expectRejectedBeforeRootLookup(noIndexId, placeholder, vectorDeltaOptions());
	expectRejectedBeforeRootLookup(vectorDefinition(), emptyMutations, vectorDeltaOptions());
	NativeGraphVectorDeltaOptions noRecords = vectorDeltaOptions();
	noRecords.maximum_records = 0U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noRecords);
	const std::array<NativeVectorMutation, 2U> tooManyRecords = {{placeholder.front(), placeholder.front()}};
	NativeGraphVectorDeltaOptions boundedRecords = vectorDeltaOptions();
	boundedRecords.maximum_records = 1U;
	expectRejectedBeforeRootLookup(vectorDefinition(), tooManyRecords, boundedRecords);
	NativeGraphVectorDeltaOptions noRawPages = vectorDeltaOptions();
	noRawPages.maximum_raw_pages = 0U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noRawPages);
	NativeGraphVectorDeltaOptions noFirstSlot = vectorDeltaOptions();
	noFirstSlot.first_slot = PhysicalSlotId{};
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noFirstSlot);
	NativeGraphVectorDeltaOptions noLastSlot = vectorDeltaOptions();
	noLastSlot.last_slot = PhysicalSlotId{};
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noLastSlot);
	NativeGraphVectorDeltaOptions noRebuildThreshold = vectorDeltaOptions();
	noRebuildThreshold.rebuild_threshold = 0U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noRebuildThreshold);
	NativeGraphVectorDeltaOptions unboundedRebuildThreshold = vectorDeltaOptions();
	unboundedRebuildThreshold.rebuild_threshold = std::numeric_limits<size_t>::max();
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, unboundedRebuildThreshold);
	NativeGraphVectorDeltaOptions wideRebuildThreshold = vectorDeltaOptions();
	wideRebuildThreshold.rebuild_threshold = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, wideRebuildThreshold);
	NativeGraphVectorDeltaOptions invalidThreshold = vectorDeltaOptions();
	invalidThreshold.rebuild_threshold = invalidThreshold.maximum_overlay_entries + 1U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, invalidThreshold);
	NativeGraphVectorDeltaOptions noOverlay = vectorDeltaOptions();
	noOverlay.maximum_overlay_entries = 0U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noOverlay);
	NativeGraphVectorDeltaOptions unboundedOverlay = vectorDeltaOptions();
	unboundedOverlay.maximum_overlay_entries = std::numeric_limits<size_t>::max();
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, unboundedOverlay);
	NativeGraphVectorDeltaOptions wideOverlay = vectorDeltaOptions();
	wideOverlay.maximum_overlay_entries = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, wideOverlay);
	NativeGraphVectorDeltaOptions noDeltaPages = vectorDeltaOptions();
	noDeltaPages.maximum_delta_pages = 0U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, noDeltaPages);
	NativeGraphVectorDeltaOptions unboundedDeltaPages = vectorDeltaOptions();
	unboundedDeltaPages.maximum_delta_pages = std::numeric_limits<size_t>::max();
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, unboundedDeltaPages);
	NativeGraphVectorDeltaOptions pageBoundedOverlay = vectorDeltaOptions();
	pageBoundedOverlay.maximum_delta_pages = 1U;
	pageBoundedOverlay.maximum_overlay_entries = VectorDeltaMapping::maximumEntriesForPageBudget(1U) + 1U;
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, pageBoundedOverlay);
	NativeGraphVectorDeltaOptions reversedSlots = vectorDeltaOptions();
	reversedSlots.first_slot = PhysicalSlotId(81U);
	reversedSlots.last_slot = PhysicalSlotId(80U);
	expectRejectedBeforeRootLookup(vectorDefinition(), placeholder, reversedSlots);
	IndexDefinition noDimensions = vectorDefinition();
	noDimensions.vector_dimensions = 0U;
	expectRejectedBeforeRootLookup(noDimensions, placeholder, vectorDeltaOptions());
	IndexDefinition tooManyDimensions = vectorDefinition();
	tooManyDimensions.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS + 1U;
	expectRejectedBeforeRootLookup(tooManyDimensions, placeholder, vectorDeltaOptions());

	NativeGraphWriteTransaction missingRoot = planner.beginTransaction();
	EXPECT_THROW(missingRoot.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), placeholder,
												   vectorDeltaOptions()),
				 FormatException);
	missingRoot.rollback();

	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> bootstrapRecords = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), bootstrapRecords,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction invalidKind = planner.beginTransaction();
	const std::array<NativeVectorMutation, 1U> unknownKind = {{
			{static_cast<NativeVectorMutationKind>(0U),
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {}},
	}};
	EXPECT_THROW(invalidKind.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), unknownKind,
													   vectorDeltaOptions()),
			 FormatException);
	invalidKind.rollback();

	// These fields form one public semantic-mutation contract. Validate every
	// caller-provided identity and payload shape before planning a durable delta.
	const auto expectRejectedSemanticMutation = [&](NativeVectorMutation mutation) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		const std::array<NativeVectorMutation, 1U> mutations = {{std::move(mutation)}};
		EXPECT_THROW(transaction.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
															mutations, vectorDeltaOptions()),
					 FormatException);
		EXPECT_TRUE(transaction.isActive());
		transaction.rollback();
	};
	const NativeVectorMutation validErase{NativeVectorMutationKind::NVMK_ERASE,
										 vectorDefinition().index_id,
										 nodeId,
										 vectorDefinition().vector_dimensions,
										 VectorDistanceMetric::VDM_L2,
										 {}};
	NativeVectorMutation wrongIndex = validErase;
	wrongIndex.index_id += 1U;
	expectRejectedSemanticMutation(std::move(wrongIndex));
	NativeVectorMutation zeroNode = validErase;
	zeroNode.node_id = 0U;
	expectRejectedSemanticMutation(std::move(zeroNode));
	NativeVectorMutation wrongDimensions = validErase;
	wrongDimensions.dimensions += 1U;
	expectRejectedSemanticMutation(std::move(wrongDimensions));
	NativeVectorMutation wrongMetric = validErase;
	wrongMetric.metric = VectorDistanceMetric::VDM_COSINE;
	expectRejectedSemanticMutation(std::move(wrongMetric));
	NativeVectorMutation eraseWithPayload = validErase;
	eraseWithPayload.encoded_bfloat16 = {std::byte{0x10U}};
	expectRejectedSemanticMutation(std::move(eraseWithPayload));
	NativeVectorMutation insertWithWrongWidth = validErase;
	insertWithWrongWidth.kind = NativeVectorMutationKind::NVMK_INSERT;
	insertWithWrongWidth.encoded_bfloat16 = {std::byte{0x10U}};
	expectRejectedSemanticMutation(std::move(insertWithWrongWidth));

	NativeGraphWriteTransaction duplicate = planner.beginTransaction();
	const std::array<NativeVectorMutation, 2U> duplicateErases = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {}},
	}};
	EXPECT_THROW(duplicate.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
												 duplicateErases, vectorDeltaOptions()),
				 FormatException);
	duplicate.rollback();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(current, vectorDefinition().index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorManifestOptional(current, vectorDefinition().index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(current, 2U).has_value());
	current.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsZeroNormCosineDeltaPayload) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition cosine = vectorDefinition();
	cosine.vector_metric = "cosine";
	const std::array<uint64_t, 1U> labels = {7U};
	{
		NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
		const uint64_t nodeId = bootstrap.createNode(labels);
		const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
				{nodeId, {std::byte{0x80}, std::byte{0x3F}, std::byte{0}, std::byte{0}}},
		}};
		bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, cosine, records, vectorOptions());
		ASSERT_TRUE(bootstrap.commit().committed());
	}

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t secondNode = delta.createNode(labels);
	const std::array<NativeVectorMutation, 1U> mutations = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 cosine.index_id,
			 secondNode,
			 cosine.vector_dimensions,
			 VectorDistanceMetric::VDM_COSINE,
			 {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}},
	}};
	EXPECT_THROW(
			delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, cosine, mutations, vectorDeltaOptions()),
			FormatException);

	// Rejecting one malformed payload must not poison the transaction. A valid
	// cosine Vector for the already staged Node still publishes atomically.
	const std::array<NativeVectorMutation, 1U> validInsertion = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 cosine.index_id,
			 secondNode,
			 cosine.vector_dimensions,
			 VectorDistanceMetric::VDM_COSINE,
			 {std::byte{0x80}, std::byte{0x3F}, std::byte{0x00}, std::byte{0x40}}},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, cosine, validInsertion, vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(BTreeIndex(cosine.index_id, *indexes_, *metadata_)
				  .find(current, VectorSemanticCodec::nodeMappingKey(secondNode)),
			  std::optional<uint64_t>(2U));
	current.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsDeltaInsertForANodeOutsideTheGraphSnapshot) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 91U,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	EXPECT_THROW(delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), insertion,
											 vectorDeltaOptions()),
				 FormatException);
	delta.rollback();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsDeltaAfterStagingBootstrapForTheSameIndex) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										  vectorOptions());
	const std::array<NativeVectorMutation, 1U> mutations = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	EXPECT_THROW(transaction.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations,
												   vectorDeltaOptions()),
				 FormatException);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsSecondBootstrapForTheSameIndexInOneTransaction) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										  vectorOptions());
	EXPECT_THROW(transaction.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
													   vectorOptions()),
				 FormatException);
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsSecondDeltaForTheSameIndexInOneTransaction) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{baseNodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t insertedNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 insertedNodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), insertion,
								vectorDeltaOptions());
	EXPECT_THROW(delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), insertion,
											 vectorDeltaOptions()),
				 FormatException);
	delta.rollback();
}

TEST_F(NativeGraphWritePlannerVectorTest, PublishesLogicalCreateAndReplacementThroughNativeVectorState) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *indexes_);
	NativeGraphMutationTokenResolvers tokens;
	tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
							 std::string_view name) -> std::optional<uint64_t> {
		return kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL && name == "Person" ? std::optional<uint64_t>(7U)
																					 : std::nullopt;
	};
	tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
		return name == "embedding" ? std::optional<uint32_t>(17U) : std::nullopt;
	};
	tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
		return token == 17U ? std::optional<std::string>("embedding") : std::nullopt;
	};
	NativeGraphMutationExecutor executor(
			planner, graph, *indexes_, *raw_pages_, *manifests_, *metadata_, std::move(tokens),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{vectorDefinition()}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"},
						   {{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																				   PropertyValue(int64_t(2))})}}}};
	const NativeGraphMutationExecutionResult created = executor.execute(batch);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 1U);
	const uint64_t nodeId = created.result.created_node_ids.front();

	MetadataSnapshotPin indexed = metadata_->openSnapshot();
	BTreeIndex mappings(vectorDefinition().index_id, *indexes_, *metadata_);
	EXPECT_EQ(mappings.find(indexed, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(1U));
	EXPECT_TRUE(metadata_->findVectorManifestOptional(indexed, vectorDefinition().index_id).has_value());
	const VectorLocation firstLocation = metadata_->findVectorLocation(indexed, 1U);
	const VectorPage firstPage = raw_pages_->read(indexed, firstLocation.page_id);
	EXPECT_EQ(LittleEndianCodec::readU64(firstPage.read(firstLocation.slot_id, firstLocation.slot_generation), 0U), 1U);

	LogicalGraphMutationBatch replace;
	replace.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"embedding",
			   PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(3)), PropertyValue(int64_t(4))})}}}};
	const NativeGraphMutationExecutionResult replaced = executor.execute(replace);
	ASSERT_TRUE(replaced.committed());

	EXPECT_EQ(mappings.find(indexed, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(1U));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(indexed, 1U).has_value());
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(mappings.find(after, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(2U));
	EXPECT_FALSE(metadata_->findVectorLocationOptional(after, 1U).has_value());
	const VectorLocation secondLocation = metadata_->findVectorLocation(after, 2U);
	const VectorPage secondPage = raw_pages_->read(after, secondLocation.page_id);
	EXPECT_EQ(LittleEndianCodec::readU64(secondPage.read(secondLocation.slot_id, secondLocation.slot_generation), 0U),
			  2U);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(before, nodeId).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(before, vectorDefinition().index_id).has_value());
	indexed.release();
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, MaintainsNativeVectorMappingsAcrossLabelLossGainAndNodeDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *indexes_);
	NativeGraphMutationTokenResolvers tokens;
	tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
							 std::string_view name) -> std::optional<uint64_t> {
		return kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL && name == "Person" ? std::optional<uint64_t>(7U)
																					 : std::nullopt;
	};
	tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
		return name == "embedding" ? std::optional<uint32_t>(17U) : std::nullopt;
	};
	tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
		return token == 17U ? std::optional<std::string>("embedding") : std::nullopt;
	};
	NativeGraphMutationExecutor executor(
			planner, graph, *indexes_, *raw_pages_, *manifests_, *metadata_, std::move(tokens),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{vectorDefinition()}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"},
							{{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																					PropertyValue(int64_t(2))})}}}};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	const uint64_t nodeId = created.result.created_node_ids.front();

	MetadataSnapshotPin beforeLabelLoss = metadata_->openSnapshot();
	const VectorManifest initialManifest = manifests_->read(beforeLabelLoss, vectorDefinition().index_id).manifest();
	BTreeIndex nodeMappings(vectorDefinition().index_id, *indexes_, *metadata_);
	BTreeIndex vectorMappings(initialManifest.vector_node_mapping_index_id, *indexes_, *metadata_);
	EXPECT_EQ(nodeMappings.find(beforeLabelLoss, VectorSemanticCodec::nodeMappingKey(nodeId)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(vectorMappings.find(beforeLabelLoss, VectorNodeMapping::key(1U)), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(beforeLabelLoss, 1U).has_value());

	LogicalGraphMutationBatch removeLabel;
	removeLabel.node_label_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {}}};
	ASSERT_TRUE(executor.execute(removeLabel).committed());
	MetadataSnapshotPin labelRemoved = metadata_->openSnapshot();
	EXPECT_FALSE(nodeMappings.find(labelRemoved, VectorSemanticCodec::nodeMappingKey(nodeId)).has_value());
	EXPECT_FALSE(vectorMappings.find(labelRemoved, VectorNodeMapping::key(1U)).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(labelRemoved, 1U).has_value());
	EXPECT_EQ(nodeMappings.find(beforeLabelLoss, VectorSemanticCodec::nodeMappingKey(nodeId)),
			  std::optional<uint64_t>(1U));

	LogicalGraphMutationBatch restoreLabel;
	restoreLabel.node_label_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {"Person"}}};
	ASSERT_TRUE(executor.execute(restoreLabel).committed());
	MetadataSnapshotPin restored = metadata_->openSnapshot();
	EXPECT_EQ(nodeMappings.find(restored, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(2U));
	EXPECT_EQ(vectorMappings.find(restored, VectorNodeMapping::key(2U)), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(restored, 2U).has_value());

	LogicalGraphMutationBatch eraseNode;
	eraseNode.node_deletes = {{nodeId, false, 0U}};
	ASSERT_TRUE(executor.execute(eraseNode).committed());
	MetadataSnapshotPin deleted = metadata_->openSnapshot();
	EXPECT_FALSE(nodeMappings.find(deleted, VectorSemanticCodec::nodeMappingKey(nodeId)).has_value());
	EXPECT_FALSE(vectorMappings.find(deleted, VectorNodeMapping::key(2U)).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(deleted, 2U).has_value());
	EXPECT_THROW(static_cast<void>(graph.readNode(deleted, nodeId)), FormatException);

	deleted.release();
	restored.release();
	labelRemoved.release();
	beforeLabelLoss.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsVectorInsertionForANodeErasedByTheSameTransaction) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	bootstrap.stageVectorIndexBootstrap(
			*indexes_, *raw_pages_, *manifests_, vectorDefinition(),
			std::array<NativeGraphVectorBootstrapRecord, 1U>{{
					{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
			}},
			vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.eraseNode(nodeId);
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 vectorDefinition().vector_dimensions,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	// A delete and a replacement vector cannot be folded into a durable state for
	// the same Node. Reject it while the write remains rollback-safe.
	EXPECT_THROW(transaction.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), insertion,
											 vectorDeltaOptions()),
			 FormatException);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	transaction.rollback();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock(), beforeSuperblock);
	EXPECT_EQ(BTreeIndex(vectorDefinition().index_id, *indexes_, *metadata_)
				  .find(after, VectorSemanticCodec::nodeMappingKey(nodeId)),
			  std::optional<uint64_t>(1U));
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, ReplacesMappingsRawLocationAndManifestInOneDeltaGeneration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const VectorManifest oldManifest = manifests_->read(before, vectorDefinition().index_id).manifest();
	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 2U> mutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations, [&] {
		NativeGraphVectorDeltaOptions options = vectorDeltaOptions();
		// Crossing the soft threshold must publish a query-correct
		// overlay and return a maintenance request, not reject the
		// graph mutation.
		options.rebuild_threshold = 1U;
		return options;
	}());
	const NativeGraphWriteCommitResult committed = delta.commit();
	ASSERT_TRUE(committed.committed());
	EXPECT_EQ(committed.vector_rebuild_index_ids, std::vector<uint64_t>{vectorDefinition().index_id});
	EXPECT_EQ(committed.superblock.generation, Generation(4U));

	BTreeIndex oldNodeMapping(vectorDefinition().index_id, *indexes_, *metadata_);
	EXPECT_EQ(oldNodeMapping.find(before, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(1U));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(before, 1U).has_value());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	BTreeIndex nodeMapping(vectorDefinition().index_id, *indexes_, *metadata_);
	const VectorManifest newManifest = manifests_->read(after, vectorDefinition().index_id).manifest();
	EXPECT_EQ(nodeMapping.find(after, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(2U));
	EXPECT_FALSE(metadata_->findVectorLocationOptional(after, 1U).has_value());
	const VectorLocation location = metadata_->findVectorLocation(after, 2U);
	const VectorPage page = raw_pages_->read(after, location.page_id);
	EXPECT_EQ(LittleEndianCodec::readU64(page.read(location.slot_id, location.slot_generation), 0U), 2U);
	// A mapping-root PageId is logical and can stay stable while its physical
	// Page Directory frame is replaced by COW; mapping contents above prove the
	// new root image is the one visible to this snapshot.
	EXPECT_GT(newManifest.committed_lsn.value(), oldManifest.committed_lsn.value());
	EXPECT_EQ(newManifest.raw_write_tail_page_id, oldManifest.raw_write_tail_page_id);
	ASSERT_NE(newManifest.delta_mapping_index_id, 0U);
	EXPECT_EQ(newManifest.delta_entry_count, 2U);
	EXPECT_EQ(VectorDeltaMapping::findOperation(after, *indexes_, *metadata_, newManifest.delta_mapping_index_id, 1U),
			  std::optional(VectorDeltaOperation::VDO_ERASE));
	EXPECT_EQ(VectorDeltaMapping::findOperation(after, *indexes_, *metadata_, newManifest.delta_mapping_index_id, 2U),
			  std::optional(VectorDeltaOperation::VDO_INSERT));
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));
	const auto rawEntry = std::ranges::find_if(newManifest.entries, [&](const VectorManifestEntry &entry) {
		return entry.page_id == location.page_id && entry.storage_kind == VectorStorageKind::VSK_RAW;
	});
	ASSERT_NE(rawEntry, newManifest.entries.end());
	EXPECT_EQ(rawEntry->record_count, page.header().live_count);
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, PublishesAdditionalRawPagesWhenDeltaOutgrowsAFullTail) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 79U;
	definition.name = "native-vector-wide";
	definition.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS;
	const std::vector<std::byte> encoded(static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t),
										 std::byte{0x3F});
	ASSERT_EQ(VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(sizeof(uint64_t) + encoded.size())), 1U);

	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> baseRecords = {{{baseNodeId, encoded}}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, baseRecords, vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const VectorManifest beforeManifest = manifests_->read(before, definition.index_id).manifest();
	const VectorLocation baseLocation = metadata_->findVectorLocation(before, 1U);
	EXPECT_EQ(raw_pages_->read(before, baseLocation.page_id).header().slot_capacity, 1U);
	EXPECT_EQ(raw_pages_->read(before, baseLocation.page_id).header().live_count, 1U);

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t firstNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 2U> insertions = {{
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, firstNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, secondNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, insertions, vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const VectorManifest afterManifest = manifests_->read(after, definition.index_id).manifest();
	const VectorLocation firstLocation = metadata_->findVectorLocation(after, 2U);
	const VectorLocation secondLocation = metadata_->findVectorLocation(after, 3U);
	EXPECT_NE(firstLocation.page_id, baseLocation.page_id);
	EXPECT_NE(secondLocation.page_id, baseLocation.page_id);
	EXPECT_NE(firstLocation.page_id, secondLocation.page_id);
	EXPECT_EQ(raw_pages_->read(after, firstLocation.page_id).header().live_count, 1U);
	EXPECT_EQ(raw_pages_->read(after, secondLocation.page_id).header().live_count, 1U);
	EXPECT_GT(afterManifest.entries.size(), beforeManifest.entries.size());
	EXPECT_EQ(BTreeIndex(definition.index_id, *indexes_, *metadata_)
					  .find(after, VectorSemanticCodec::nodeMappingKey(firstNodeId)),
			  std::optional<uint64_t>(2U));
	EXPECT_EQ(BTreeIndex(definition.index_id, *indexes_, *metadata_)
					  .find(after, VectorSemanticCodec::nodeMappingKey(secondNodeId)),
			  std::optional<uint64_t>(3U));

	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, AppendsAFullTailDeltaPageToItsExistingRawPool) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 80U;
	definition.name = "native-vector-raw-pool-append";
	definition.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS;
	const std::vector<std::byte> encoded(static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t),
										 std::byte{0x3F});
	ASSERT_EQ(VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(sizeof(uint64_t) + encoded.size())), 1U);

	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{{baseNodeId, encoded}}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, records, vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const VectorManifest beforeManifest = manifests_->read(before, definition.index_id).manifest();
	const PageLocation fullTail = metadata_->findPageLocation(before, beforeManifest.raw_write_tail_page_id);
	ASSERT_EQ(raw_pages_->read(before, fullTail.page_id).header().live_count, 1U);

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t insertedNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, insertedNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, insertion, vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const VectorLocation insertedLocation = metadata_->findVectorLocation(after, 2U);
	const PageLocation appendedPage = metadata_->findPageLocation(after, insertedLocation.page_id);
	EXPECT_EQ(appendedPage.segment_id, fullTail.segment_id);
	EXPECT_NE(appendedPage.page_id, fullTail.page_id);
	EXPECT_EQ(raw_pages_->read(after, insertedLocation.page_id).header().live_count, 1U);
	EXPECT_EQ(BTreeIndex(definition.index_id, *indexes_, *metadata_)
					  .find(after, VectorSemanticCodec::nodeMappingKey(insertedNodeId)),
			  std::optional<uint64_t>(2U));

	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, ReplacesAPartialRawTailAndAppendsTheRemainingDeltaInOnePool) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 81U;
	definition.name = "native-vector-partial-tail";
	definition.vector_dimensions = 800U;
	const std::vector<std::byte> encoded(static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t),
										 std::byte{0x3F});
	ASSERT_EQ(VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(sizeof(uint64_t) + encoded.size())), 2U);

	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition,
										  std::array<NativeGraphVectorBootstrapRecord, 1U>{{{baseNodeId, encoded}}},
										  vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const VectorManifest beforeManifest = manifests_->read(before, definition.index_id).manifest();
	const PageLocation oldTail = metadata_->findPageLocation(before, beforeManifest.raw_write_tail_page_id);
	EXPECT_EQ(raw_pages_->read(before, oldTail.page_id).header().live_count, 1U);

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t firstNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 2U> insertions = {{
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, firstNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, secondNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, insertions, vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const VectorLocation firstLocation = metadata_->findVectorLocation(after, 2U);
	const VectorLocation secondLocation = metadata_->findVectorLocation(after, 3U);
	const PageLocation replacedTail = metadata_->findPageLocation(after, firstLocation.page_id);
	const PageLocation appendedPage = metadata_->findPageLocation(after, secondLocation.page_id);
	EXPECT_EQ(firstLocation.page_id, beforeManifest.raw_write_tail_page_id);
	EXPECT_EQ(replacedTail.segment_id, oldTail.segment_id);
	EXPECT_EQ(appendedPage.segment_id, oldTail.segment_id);
	EXPECT_NE(appendedPage.page_id, oldTail.page_id);
	EXPECT_EQ(raw_pages_->read(after, firstLocation.page_id).header().live_count, 2U);
	EXPECT_EQ(raw_pages_->read(after, secondLocation.page_id).header().live_count, 1U);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, PublishesANewRawPoolWhenTheCurrentRawPoolIsFull) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 80U;
	definition.name = "native-vector-full-pool";
	definition.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS;
	const std::vector<std::byte> encoded(static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t),
										 std::byte{0x3F});
	ASSERT_EQ(VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(sizeof(uint64_t) + encoded.size())), 1U);

	NativeGraphVectorBootstrapOptions bootstrapOptions = vectorOptions();
	bootstrapOptions.maximum_vectors = RSS_PAGE_POOL_DATA_FRAME_COUNT;
	bootstrapOptions.maximum_pages = RSS_PAGE_POOL_DATA_FRAME_COUNT + 4U;
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	std::vector<NativeGraphVectorBootstrapRecord> baseRecords;
	baseRecords.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	for (uint32_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		baseRecords.push_back({bootstrap.createNode(std::array<uint64_t, 1U>{7U}), std::vector<std::byte>(encoded)});
	}
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, baseRecords, bootstrapOptions);
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const VectorManifest beforeManifest = manifests_->read(before, definition.index_id).manifest();
	const PageLocation fullPoolTail = metadata_->findPageLocation(before, beforeManifest.raw_write_tail_page_id);
	EXPECT_EQ(raw_pages_->read(before, fullPoolTail.page_id).header().live_count, 1U);

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t insertedNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, insertedNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, insertion, vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());

	const uint64_t insertedVectorId = static_cast<uint64_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) + 1U;
	MetadataSnapshotPin after = metadata_->openSnapshot();
	const VectorLocation insertedLocation = metadata_->findVectorLocation(after, insertedVectorId);
	const PageLocation insertedPage = metadata_->findPageLocation(after, insertedLocation.page_id);
	EXPECT_NE(insertedPage.segment_id, fullPoolTail.segment_id);
	EXPECT_EQ(raw_pages_->read(after, insertedLocation.page_id).header().live_count, 1U);
	EXPECT_EQ(BTreeIndex(definition.index_id, *indexes_, *metadata_)
					  .find(after, VectorSemanticCodec::nodeMappingKey(insertedNodeId)),
			  std::optional<uint64_t>(insertedVectorId));

	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, MovesTheManifestToANewPoolWhenCowFramesAreExhausted) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 180U;
	definition.name = "native-vector-manifest-cow-exhaustion";

	// Every delta replaces the immutable Manifest page. Fill the initial pool
	// using erase-only deltas so Raw-page placement cannot mask the Manifest path.
	NativeGraphVectorBootstrapOptions bootstrapOptions = vectorOptions();
	bootstrapOptions.maximum_vectors = RSS_PAGE_POOL_DATA_FRAME_COUNT;
	bootstrapOptions.maximum_pages = RSS_PAGE_POOL_DATA_FRAME_COUNT + 8U;
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	std::vector<uint64_t> nodeIds;
	std::vector<NativeGraphVectorBootstrapRecord> records;
	nodeIds.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	records.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	for (uint32_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		const uint64_t nodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
		nodeIds.push_back(nodeId);
		records.push_back(
				{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}});
	}
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, records, bootstrapOptions);
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphVectorDeltaOptions deltaOptions = vectorDeltaOptions();
	deltaOptions.rebuild_threshold = RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
	deltaOptions.maximum_overlay_entries = RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
	std::vector<MetadataSnapshotPin> pinnedGenerations;
	pinnedGenerations.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
	pinnedGenerations.push_back(metadata_->openSnapshot());
	for (uint32_t ordinal = 0U; ordinal + 1U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		NativeGraphWriteTransaction delta = planner.beginTransaction();
		const std::array<NativeVectorMutation, 1U> erase = {{
				{NativeVectorMutationKind::NVMK_ERASE, definition.index_id, nodeIds[ordinal], definition.vector_dimensions,
				 VectorDistanceMetric::VDM_L2, {}},
		}};
		delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, erase, deltaOptions);
		ASSERT_TRUE(delta.commit().committed());
		// Pin every complete generation so its Manifest frame cannot be reused
		// before the next COW publication consumes a different frame.
		pinnedGenerations.push_back(metadata_->openSnapshot());
	}

	const MetadataSnapshotPin &beforeMove = pinnedGenerations.back();
	const VectorManifest beforeManifest = manifests_->read(beforeMove, definition.index_id).manifest();
	const PageLocation beforeLocation = metadata_->findPageLocation(beforeMove, beforeManifest.page_id);
	const CatalogEntry beforePool = metadata_->findCatalog(beforeMove, beforeLocation.segment_id);
	ASSERT_EQ(beforePool.kind, SegmentKind::SGK_VECTOR_MANIFEST_POOL);
	ASSERT_FALSE(manifests_->hasReplacementFrameCapacity(beforePool, 1U));
	EXPECT_EQ(VectorDeltaMapping::findOperation(beforeMove, *indexes_, *metadata_, beforeManifest.delta_mapping_index_id,
												 RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U),
			  std::optional(VectorDeltaOperation::VDO_ERASE));

	NativeGraphWriteTransaction finalDelta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 1U> finalErase = {{
			{NativeVectorMutationKind::NVMK_ERASE, definition.index_id, nodeIds.back(), definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, {}},
	}};
	finalDelta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, finalErase, deltaOptions);
	ASSERT_TRUE(finalDelta.commit().committed());

	MetadataSnapshotPin afterMove = metadata_->openSnapshot();
	const VectorManifest afterManifest = manifests_->read(afterMove, definition.index_id).manifest();
	const PageLocation afterLocation = metadata_->findPageLocation(afterMove, afterManifest.page_id);
	EXPECT_EQ(afterManifest.page_id, beforeManifest.page_id);
	EXPECT_NE(afterLocation.segment_id, beforeLocation.segment_id);
	EXPECT_EQ(metadata_->findCatalog(afterMove, afterLocation.segment_id).kind, SegmentKind::SGK_VECTOR_MANIFEST_POOL);
	EXPECT_FALSE(BTreeIndex(definition.index_id, *indexes_, *metadata_)
				 .find(afterMove, VectorSemanticCodec::nodeMappingKey(nodeIds.back()))
				 .has_value());
	// The previous complete Manifest and delta tree remain readable through the pinned snapshot.
	EXPECT_TRUE(BTreeIndex(definition.index_id, *indexes_, *metadata_)
				.find(beforeMove, VectorSemanticCodec::nodeMappingKey(nodeIds.back()))
				.has_value());
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));

	afterMove.release();
	for (MetadataSnapshotPin &snapshot: pinnedGenerations) {
		snapshot.release();
	}
	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const VectorManifest recoveredManifest = manifests_->read(recovered, definition.index_id).manifest();
	const PageLocation recoveredLocation = metadata_->findPageLocation(recovered, recoveredManifest.page_id);
	EXPECT_EQ(recoveredLocation.segment_id, afterLocation.segment_id);
	EXPECT_FALSE(BTreeIndex(definition.index_id, *indexes_, *metadata_)
				 .find(recovered, VectorSemanticCodec::nodeMappingKey(nodeIds.back()))
				 .has_value());
	recovered.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsDeltaThatExceedsItsRawPageBudgetBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition definition = vectorDefinition();
	definition.index_id = 81U;
	definition.name = "native-vector-raw-page-bound";
	definition.vector_dimensions = RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS;
	const std::vector<std::byte> encoded(static_cast<size_t>(definition.vector_dimensions) * sizeof(uint16_t),
										 std::byte{0x3F});
	ASSERT_EQ(VectorPageStore::rawSlotCapacity(static_cast<uint32_t>(sizeof(uint64_t) + encoded.size())), 1U);

	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeGraphVectorBootstrapRecord, 1U> baseRecords = {{{baseNodeId, encoded}}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, definition, baseRecords, vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation beforeGeneration = before.superblock().generation;
	before.release();
	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const uint64_t firstNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNodeId = delta.createNode(std::array<uint64_t, 1U>{7U});
	const std::array<NativeVectorMutation, 2U> insertions = {{
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, firstNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
			{NativeVectorMutationKind::NVMK_INSERT, definition.index_id, secondNodeId, definition.vector_dimensions,
			 VectorDistanceMetric::VDM_L2, encoded},
	}};
	NativeGraphVectorDeltaOptions limited = vectorDeltaOptions();
	limited.maximum_raw_pages = 1U;
	EXPECT_THROW(delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, definition, insertions, limited),
				 FormatException);
	delta.rollback();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, beforeGeneration);
	EXPECT_FALSE(metadata_->findVectorLocationOptional(after, 2U).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(after, 3U).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(after, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(after, secondNodeId).has_value());
	after.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RejectsDeltaThatExceedsTheHardOverlayCapBeforePublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 2U> mutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	NativeGraphVectorDeltaOptions bounded = vectorDeltaOptions();
	bounded.rebuild_threshold = 1U;
	bounded.maximum_overlay_entries = 1U;
	EXPECT_THROW(
			delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations, bounded),
			FormatException);
	delta.rollback();

	BTreeIndex mapping(vectorDefinition().index_id, *indexes_, *metadata_);
	EXPECT_EQ(mapping.find(before, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(1U));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(before, 1U).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(before, 2U).has_value());
	before.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, PublishesTwoVectorDeltaRangesInGlobalVectorIdOrder) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	IndexDefinition firstDefinition = vectorDefinition();
	IndexDefinition secondDefinition = vectorDefinition();
	secondDefinition.index_id = 78U;
	secondDefinition.name = "native-vector-second";
	secondDefinition.property_names = {"embedding-second"};
	const std::array<uint64_t, 1U> labels = {7U};

	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> firstRecords = {{
			{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	const std::array<NativeGraphVectorBootstrapRecord, 1U> secondRecords = {{
			{nodeId, {std::byte{0x20}, std::byte{0x40}, std::byte{0x60}, std::byte{0x80}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, firstDefinition, firstRecords,
										vectorOptions());
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, secondDefinition, secondRecords,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 2U> secondMutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 secondDefinition.index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 secondDefinition.index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x22}, std::byte{0x42}, std::byte{0x62}, std::byte{0x82}}},
	}};
	const std::array<NativeVectorMutation, 2U> firstMutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 firstDefinition.index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 firstDefinition.index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x11}, std::byte{0x31}, std::byte{0x51}, std::byte{0x71}}},
	}};
	// Stage the larger Index ID first. Metadata validation must sort the two
	// reserved Vector-ID ranges rather than assuming Index order is allocation order.
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, secondDefinition, secondMutations,
								vectorDeltaOptions());
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, firstDefinition, firstMutations,
								vectorDeltaOptions());
	const NativeGraphWriteCommitResult committed = delta.commit();
	ASSERT_TRUE(committed.committed());

	const MetadataSnapshotPin current = metadata_->openSnapshot();
	const VectorManifest firstManifest = manifests_->read(current, firstDefinition.index_id).manifest();
	const VectorManifest secondManifest = manifests_->read(current, secondDefinition.index_id).manifest();
	EXPECT_EQ(firstManifest.delta_entry_count, 2U);
	EXPECT_EQ(secondManifest.delta_entry_count, 2U);
	EXPECT_EQ(BTreeIndex(firstDefinition.index_id, *indexes_, *metadata_)
					  .find(current, VectorSemanticCodec::nodeMappingKey(nodeId)),
			  std::optional<uint64_t>(4U));
	EXPECT_EQ(BTreeIndex(secondDefinition.index_id, *indexes_, *metadata_)
					  .find(current, VectorSemanticCodec::nodeMappingKey(nodeId)),
			  std::optional<uint64_t>(3U));
}

TEST_F(NativeGraphWritePlannerVectorTest, RetiresDeltaMappingWhenAnOverlayOnlyVectorIsErased) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t baseNodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{baseNodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	NativeGraphWriteTransaction insert = planner.beginTransaction();
	const uint64_t overlayNodeId = insert.createNode(labels);
	const std::array<NativeVectorMutation, 1U> insertion = {{
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 overlayNodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};
	insert.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), insertion,
								 vectorDeltaOptions());
	ASSERT_TRUE(insert.commit().committed());
	MetadataSnapshotPin withOverlay = metadata_->openSnapshot();
	const VectorManifest overlayManifest = manifests_->read(withOverlay, vectorDefinition().index_id).manifest();
	ASSERT_NE(overlayManifest.delta_mapping_index_id, 0U);
	ASSERT_EQ(overlayManifest.delta_entry_count, 1U);
	EXPECT_EQ(VectorDeltaMapping::findOperation(withOverlay, *indexes_, *metadata_,
												overlayManifest.delta_mapping_index_id, 2U),
			  std::optional(VectorDeltaOperation::VDO_INSERT));

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	const std::array<NativeVectorMutation, 1U> erasure = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 overlayNodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
	}};
	erase.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), erasure, vectorDeltaOptions());
	ASSERT_TRUE(erase.commit().committed());

	const MetadataSnapshotPin current = metadata_->openSnapshot();
	const VectorManifest currentManifest = manifests_->read(current, vectorDefinition().index_id).manifest();
	EXPECT_EQ(currentManifest.delta_mapping_index_id, 0U);
	EXPECT_FALSE(currentManifest.delta_mapping_root_page_id.isValid());
	EXPECT_FALSE(currentManifest.delta_mapping_root_generation.isValid());
	EXPECT_EQ(currentManifest.delta_entry_count, 0U);
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, overlayManifest.delta_mapping_index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(current, 2U).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(current, 1U).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(withOverlay, overlayManifest.delta_mapping_index_id).has_value());
	withOverlay.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, PreservesDerivedPqStateWithSnapshotSafeBaseDeltaOverlay) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
	const std::array<uint64_t, 1U> labels = {7U};
	NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
	const uint64_t nodeId = bootstrap.createNode(labels);
	const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
			{nodeId, {std::byte{0x80}, std::byte{0x3F}, std::byte{0x00}, std::byte{0x40}}},
	}};
	bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
										vectorOptions());
	ASSERT_TRUE(bootstrap.commit().committed());

	const std::array<DeterministicPqTrainingRecord, 1U> training = {{
			{1U, records.front().encoded_bfloat16},
	}};
	const OfflineVectorPqRebuildResult rebuilt =
			OfflineVectorPqRebuild::run(*indexes_, *raw_pages_, *codebooks_, *manifests_, *metadata_,
										vectorDefinition().index_id, training, pqRebuildOptions());
	ASSERT_TRUE(rebuilt.superblock.generation.isValid());

	MetadataSnapshotPin derived = metadata_->openSnapshot();
	const VectorManifest derivedManifest = manifests_->read(derived, vectorDefinition().index_id).manifest();
	ASSERT_NE(derivedManifest.codebook_id, 0U);
	ASSERT_NE(derivedManifest.pq_mapping_index_id, 0U);
	ASSERT_TRUE(derivedManifest.pq_mapping_root_page_id.isValid());
	ASSERT_TRUE(std::ranges::any_of(derivedManifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	}));

	NativeGraphWriteTransaction delta = planner.beginTransaction();
	const std::array<NativeVectorMutation, 2U> mutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x40}, std::byte{0x40}, std::byte{0x80}, std::byte{0x40}}},
	}};
	delta.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations,
								vectorDeltaOptions());
	ASSERT_TRUE(delta.commit().committed());

	// The pinned reader retains the complete base generation. The current
	// generation retains that base and carries only the semantic delta overlay.
	EXPECT_NE(derivedManifest.codebook_id, 0U);
	EXPECT_NE(derivedManifest.pq_mapping_index_id, 0U);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	const VectorManifest currentManifest = manifests_->read(current, vectorDefinition().index_id).manifest();
	EXPECT_EQ(currentManifest.codebook_id, derivedManifest.codebook_id);
	EXPECT_EQ(currentManifest.codebook_generation, derivedManifest.codebook_generation);
	EXPECT_EQ(currentManifest.pq_mapping_index_id, derivedManifest.pq_mapping_index_id);
	EXPECT_EQ(currentManifest.pq_mapping_root_page_id, derivedManifest.pq_mapping_root_page_id);
	EXPECT_EQ(currentManifest.pq_mapping_root_generation, derivedManifest.pq_mapping_root_generation);
	EXPECT_TRUE(std::ranges::any_of(currentManifest.entries, [](const VectorManifestEntry &entry) {
		return entry.storage_kind == VectorStorageKind::VSK_PQ;
	}));
	EXPECT_EQ(currentManifest.entry_point_vector_id, derivedManifest.entry_point_vector_id);
	EXPECT_EQ(currentManifest.adjacency_generation, derivedManifest.adjacency_generation);
	EXPECT_EQ(currentManifest.adjacency_mapping_index_id, derivedManifest.adjacency_mapping_index_id);
	EXPECT_EQ(currentManifest.adjacency_mapping_root_page_id, derivedManifest.adjacency_mapping_root_page_id);
	EXPECT_EQ(currentManifest.adjacency_mapping_root_generation, derivedManifest.adjacency_mapping_root_generation);
	ASSERT_NE(currentManifest.delta_mapping_index_id, 0U);
	EXPECT_EQ(currentManifest.delta_entry_count, 2U);
	EXPECT_EQ(VectorDeltaMapping::findOperation(current, *indexes_, *metadata_, currentManifest.delta_mapping_index_id,
												1U),
			  std::optional(VectorDeltaOperation::VDO_ERASE));
	EXPECT_EQ(VectorDeltaMapping::findOperation(current, *indexes_, *metadata_, currentManifest.delta_mapping_index_id,
												2U),
			  std::optional(VectorDeltaOperation::VDO_INSERT));

	BTreeIndex nodeMapping(vectorDefinition().index_id, *indexes_, *metadata_);
	EXPECT_EQ(nodeMapping.find(current, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(2U));
	const VectorLocation location = metadata_->findVectorLocation(current, 2U);
	const VectorPage raw = raw_pages_->read(current, location.page_id);
	const std::vector<std::byte> exact = raw.read(location.slot_id, location.slot_generation);
	ASSERT_EQ(exact.size(), sizeof(uint64_t) + mutations.back().encoded_bfloat16.size());
	EXPECT_EQ(LittleEndianCodec::readU64(exact, 0U), 2U);
	EXPECT_EQ(std::vector<std::byte>(exact.begin() + static_cast<std::ptrdiff_t>(sizeof(uint64_t)), exact.end()),
			  mutations.back().encoded_bfloat16);

	VectorAdjacencyPageStore adjacencyPages(io_, *metadata_);
	DerivedObjectReachabilityReclaimer reclaimer(*indexes_, *raw_pages_, adjacencyPages, *manifests_, *metadata_);
	const DerivedObjectReachabilityReclaimOptions reclaimOptions{16U, 16U, 16U, 16U, 16U, 16U, 16U};
	const DerivedObjectReachabilityReclaimReport reclaimReport =
			reclaimer.reclaimPq(reclaimOptions, LogSequenceNumber(current.superblock().checkpoint_lsn.value() + 1U));
	EXPECT_EQ(reclaimReport.vector_locations, 1U);
	EXPECT_EQ(reclaimReport.reachable_pq_pages, 1U);
	EXPECT_EQ(reclaimReport.derived_pq_pages_retired, 0U);
	EXPECT_EQ(reclaimReport.published_generation, reclaimReport.scanned_generation);
	current.release();
	derived.release();
}

TEST_F(NativeGraphWritePlannerVectorTest, RecoversNativeVectorDeltaAcrossSuperblockInterruptions) {
	const std::array<uint64_t, 1U> labels = {7U};
	uint64_t nodeId = 0U;
	{
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
		NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
		nodeId = bootstrap.createNode(labels);
		const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
				{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
		}};
		bootstrap.stageVectorIndexBootstrap(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), records,
											vectorOptions());
		ASSERT_TRUE(bootstrap.commit().committed());
	}
	const std::array<NativeVectorMutation, 2U> mutations = {{
			{NativeVectorMutationKind::NVMK_ERASE,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {}},
			{NativeVectorMutationKind::NVMK_INSERT,
			 vectorDefinition().index_id,
			 nodeId,
			 2U,
			 VectorDistanceMetric::VDM_L2,
			 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
	}};

	{
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
		NativeGraphWriteTransaction interrupted = planner.beginTransaction();
		interrupted.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations,
										  vectorDeltaOptions());
		MetadataPublishOptions publish;
		publish.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interrupted.commit(publish)), BootstrapPublishInterrupted);
		EXPECT_EQ(interrupted.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}
	reopenNativeStores();
	{
		MetadataSnapshotPin recovered = metadata_->openSnapshot();
		BTreeIndex mapping(vectorDefinition().index_id, *indexes_, *metadata_);
		EXPECT_EQ(mapping.find(recovered, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(1U));
		EXPECT_TRUE(metadata_->findVectorLocationOptional(recovered, 1U).has_value());
		EXPECT_FALSE(metadata_->findVectorLocationOptional(recovered, 2U).has_value());
		recovered.release();
	}

	{
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *indexes_, plannerOptions());
		NativeGraphWriteTransaction interrupted = planner.beginTransaction();
		interrupted.stageVectorIndexDelta(*indexes_, *raw_pages_, *manifests_, vectorDefinition(), mutations,
										  vectorDeltaOptions());
		MetadataPublishOptions publish;
		publish.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interrupted.commit(publish)), BootstrapPublishInterrupted);
		EXPECT_EQ(interrupted.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}
	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	BTreeIndex mapping(vectorDefinition().index_id, *indexes_, *metadata_);
	EXPECT_EQ(mapping.find(recovered, VectorSemanticCodec::nodeMappingKey(nodeId)), std::optional<uint64_t>(2U));
	EXPECT_FALSE(metadata_->findVectorLocationOptional(recovered, 1U).has_value());
	const VectorLocation location = metadata_->findVectorLocation(recovered, 2U);
	const VectorPage raw = raw_pages_->read(recovered, location.page_id);
	EXPECT_EQ(LittleEndianCodec::readU64(raw.read(location.slot_id, location.slot_generation), 0U), 2U);
	const VectorManifest manifest = manifests_->read(recovered, vectorDefinition().index_id).manifest();
	EXPECT_EQ(manifest.node_mapping_root_page_id,
			  metadata_->findIndexRoot(recovered, vectorDefinition().index_id).root_page_id);
	EXPECT_EQ(manifest.vector_node_mapping_root_page_id,
			  metadata_->findIndexRoot(recovered, manifest.vector_node_mapping_index_id).root_page_id);
	recovered.release();
}
