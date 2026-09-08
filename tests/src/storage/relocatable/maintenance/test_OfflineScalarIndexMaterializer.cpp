/**
 * @file test_OfflineScalarIndexMaterializer.cpp
 * @brief Semantic native relocatable scalar-index materialization tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/maintenance/OfflineScalarIndexMaterializer.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"

using namespace graph;
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

} // namespace

class RelocatableOfflineScalarIndexMaterializerTest : public ::testing::Test {
protected:
	std::filesystem::path destination_path;
	std::filesystem::path wal_path;
	std::shared_ptr<std::fstream> destination_file;
	std::shared_ptr<std::fstream> wal_file;
	std::shared_ptr<StorageIO> destination_io;
	std::shared_ptr<StorageIO> wal_io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		const std::filesystem::path prefix =
				std::filesystem::temp_directory_path() / ("relocatable_scalar_indexes_" + to_string(uuid));
		destination_path = prefix.string() + ".relocatable";
		wal_path = destination_path.string() + ".foreign";
		destination_file = std::make_shared<std::fstream>(
				destination_path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file = std::make_shared<std::fstream>(wal_path,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(destination_file->is_open());
		ASSERT_TRUE(wal_file->is_open());
		destination_io = std::make_shared<StorageIO>(destination_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io = std::make_shared<StorageIO>(wal_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		destination_io.reset();
		wal_io.reset();
		for (const std::shared_ptr<std::fstream> &file: {destination_file, wal_file}) {
			if (file && file->is_open()) {
				file->close();
			}
		}
		destination_file.reset();
		wal_file.reset();
		std::error_code error;
		std::filesystem::remove(destination_path, error);
		std::filesystem::remove(wal_path, error);
	}

	[[nodiscard]] static LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	[[nodiscard]] static NativeGraphWritePlannerOptions catalogPlannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(62U);
		options.last_slot = PhysicalSlotId(70U);
		options.entity_pool_owner_id = 402U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
		return options;
	}

	static void publishPropertyTokens(SystemCatalog &catalog, NativeGraphWritePlanner &planner,
								  std::initializer_list<std::string_view> names) {
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		for (const std::string_view name: names) {
			static_cast<void>(session.ensurePropertyToken(name));
		}
		session.finish();
		ASSERT_TRUE(transaction.commit().committed());
	}

	void populateNative(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, SlottedPagePool &pool) {
		EntityPageStore entities(io, metadata);
		NodeEntityRecord aliceNode;
		aliceNode.inline_label_ids[0] = 11U;
		aliceNode.inline_label_count = 1U;
		NodeEntityRecord bobNode;
		bobNode.inline_label_ids[0] = 12U;
		bobNode.inline_label_count = 1U;
		ASSERT_TRUE(entities.allocateNode(
														 {.segment_id = SegmentId(101U),
														  .pool_owner_id = 11U,
														  .generation = Generation(1U),
														  .page_id = PageId(101U),
														  .first_slot = PhysicalSlotId(60U),
														  .last_slot = PhysicalSlotId(60U)},
														 1U, aliceNode, nextCheckpointLsn(metadata))
													.has_value());
		{
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			static_cast<void>(entities.insertNode(snapshot, PageId(101U), 2U, bobNode, nextCheckpointLsn(metadata)));
		}
		EdgeEntityRecord aliceEdge;
		aliceEdge.source_node_id = 1U;
		aliceEdge.target_node_id = 2U;
		aliceEdge.type_id = 21U;
		EdgeEntityRecord untypedEdge = aliceEdge;
		untypedEdge.type_id = RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN;
		ASSERT_TRUE(entities.allocateEdge(
														 {.segment_id = SegmentId(102U),
														  .pool_owner_id = 21U,
														  .generation = Generation(1U),
														  .page_id = PageId(201U),
														  .first_slot = PhysicalSlotId(61U),
														  .last_slot = PhysicalSlotId(61U)},
														 1U, aliceEdge, nextCheckpointLsn(metadata))
													.has_value());
		{
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			static_cast<void>(entities.insertEdge(snapshot, PageId(201U), 2U, untypedEdge, nextCheckpointLsn(metadata)));
		}

		PropertyCollectionRootStore roots(pool, metadata);
		const PropertyCollectionRootStoreOptions rootOptions{PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
		PropertyCollectionRoot alice{1U, Node::typeId,
				{{1U, PropertyValue("Alice")}, {2U, PropertyValue("Able")}, {3U, PropertyValue("alice")}}};
		PropertyCollectionRoot bob{2U, Node::typeId,
				{{1U, PropertyValue("Bob")}, {2U, PropertyValue("Baker")}, {3U, PropertyValue("bob")}}};
		PropertyCollectionRoot edge{1U, Edge::typeId, {{4U, PropertyValue(int64_t(7))}}};
		const std::array propertyRoots{alice, bob, edge};
		static_cast<void>(test::publishPropertyRoots(entities, roots, metadata, propertyRoots, rootOptions));
	}

	[[nodiscard]] OfflineScalarIndexMaterializationOptions indexOptions() const {
		OfflineScalarIndexMaterializationOptions result;
		result.build = {PhysicalSlotId(80), PhysicalSlotId(100), 909, 128U, 128U};
		result.sort = {128U, 128U, 4U};
		result.maximum_definitions = 16U;
		result.maximum_property_names = 128U;
		return result;
	}

	[[nodiscard]] std::vector<IndexDefinition> definitions() const {
		return {{1, "node_labels", IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_LABEL, 0, {}, 0, ""},
				{2, "edge_types", IndexDefinitionEntityKind::IDEK_EDGE, IndexDefinitionKind::IDK_LABEL, 0, {}, 0, ""},
				{3,
				 "member_name",
				 IndexDefinitionEntityKind::IDEK_NODE,
				 IndexDefinitionKind::IDK_PROPERTY,
				 11,
				 {"name"},
				 0,
				 ""},
				{4,
				 "edge_weight",
				 IndexDefinitionEntityKind::IDEK_EDGE,
				 IndexDefinitionKind::IDK_PROPERTY,
				 0,
				 {"weight"},
				 0,
				 ""},
				{5,
				 "member_name_pair",
				 IndexDefinitionEntityKind::IDEK_NODE,
				 IndexDefinitionKind::IDK_COMPOSITE,
				 0,
				 {"first", "last"},
				 0,
				 ""},
				{6,
				 "missing_property",
				 IndexDefinitionEntityKind::IDEK_NODE,
				 IndexDefinitionKind::IDK_PROPERTY,
				 0,
				 {"missing"},
				 0,
				 ""},
				{7,
				 "node_weight",
				 IndexDefinitionEntityKind::IDEK_NODE,
				 IndexDefinitionKind::IDK_PROPERTY,
				 0,
				 {"weight"},
				 0,
				 ""},
				{8,
				 "embedding",
				 IndexDefinitionEntityKind::IDEK_NODE,
				 IndexDefinitionKind::IDK_VECTOR,
				 11,
				 {"embedding"},
				 3,
				 "l2"}};
	}
};

TEST_F(RelocatableOfflineScalarIndexMaterializerTest, MaterializesAndResumesLogicalIndexesFromNativeRecords) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	populateNative(destination_io, *metadata, pool);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, roots, pages, catalogPlannerOptions());
	publishPropertyTokens(catalog, planner, {"first", "last", "name", "weight"});

	const std::vector<IndexDefinition> definitions = this->definitions();
	const OfflineScalarIndexMaterializationResult first = OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions());
	EXPECT_EQ(first.materialized_indexes, 7U);
	EXPECT_EQ(first.verified_indexes, 0U);
	EXPECT_EQ(first.deferred_vector_indexes, 1U);
	EXPECT_EQ(first.empty_indexes_with_unassigned_property_keys, 1U);
	EXPECT_EQ(first.indexed_entries, 7U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(BTreeIndex(1, pages, *metadata).find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(11)), 1)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(BTreeIndex(2, pages, *metadata).find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(21)), 1)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(BTreeIndex(2, pages, *metadata).find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(0)), 2)),
			  std::nullopt);
	EXPECT_EQ(BTreeIndex(3, pages, *metadata).find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue("alice"), 1)),
			  std::optional<uint64_t>(1U));
	EXPECT_FALSE(BTreeIndex(3, pages, *metadata)
					 .find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue("bob"), 2))
					 .has_value());
	EXPECT_EQ(BTreeIndex(4, pages, *metadata).find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(7)), 1)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(BTreeIndex(5, pages, *metadata)
				  .find(snapshot, IndexKeyCodec::encodeCompositeEntryKey(
						  std::vector<PropertyValue>{PropertyValue("Bob"), PropertyValue("Baker")}, 2)),
			  std::optional<uint64_t>(2U));
	EXPECT_TRUE(BTreeIndex(6, pages, *metadata).scan(snapshot, std::nullopt, 1U).empty());
	EXPECT_TRUE(BTreeIndex(7, pages, *metadata).scan(snapshot, std::nullopt, 1U).empty());
	snapshot.release();

	const OfflineScalarIndexMaterializationResult resumed = OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions());
	EXPECT_EQ(resumed.materialized_indexes, 0U);
	EXPECT_EQ(resumed.verified_indexes, 7U);
	EXPECT_EQ(resumed.deferred_vector_indexes, 1U);
	EXPECT_EQ(resumed.empty_indexes_with_unassigned_property_keys, 1U);
	MetadataSnapshotPin lsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber corruptLsn(lsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	lsnSnapshot.release();
	const std::vector<std::byte> aliceKey = IndexKeyCodec::encodeEntryKey(PropertyValue("alice"), 1U);
	static_cast<void>(pages.upsert(3, aliceKey, 99U, corruptLsn));
	expectFormatError(
		[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin eraseLsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber eraseLsn(eraseLsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	eraseLsnSnapshot.release();
	ASSERT_TRUE(pages.erase(3, aliceKey, eraseLsn));
	expectFormatError(
		[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);

	// An unexpected trailing entry exhausts the expected cursor, while a changed
	// key exercises the distinct-key mismatch path. Both must fail closed.
	MetadataSnapshotPin extraLsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber extraLsn(extraLsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	extraLsnSnapshot.release();
	const std::vector<std::byte> extraLabelKey =
			IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(99)), 99U);
	static_cast<void>(pages.upsert(1, extraLabelKey, 99U, extraLsn));
	expectFormatError(
		[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin removeExtraLsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber removeExtraLsn(removeExtraLsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	removeExtraLsnSnapshot.release();
	ASSERT_TRUE(pages.erase(1, extraLabelKey, removeExtraLsn));

	MetadataSnapshotPin changedKeyLsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber changedKeyLsn(changedKeyLsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	changedKeyLsnSnapshot.release();
	const std::vector<std::byte> originalTypeKey =
			IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(21)), 1U);
	const std::vector<std::byte> changedTypeKey =
			IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(22)), 1U);
	ASSERT_TRUE(pages.erase(2, originalTypeKey, changedKeyLsn));
	MetadataSnapshotPin replacementLsnSnapshot = metadata->openSnapshot();
	const LogSequenceNumber replacementLsn(replacementLsnSnapshot.superblock().checkpoint_lsn.value() + 1U);
	replacementLsnSnapshot.release();
	static_cast<void>(pages.upsert(2, changedTypeKey, 1U, replacementLsn));
	expectFormatError(
		[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest, SkipsNodesWithoutPropertyRootsDuringPropertyMaterialization) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	populateNative(destination_io, *metadata, pool);
	EntityPageStore entities(destination_io, *metadata);
	{
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		NodeEntityRecord nodeWithoutProperties;
		nodeWithoutProperties.inline_label_ids[0] = 11U;
		nodeWithoutProperties.inline_label_count = 1U;
		static_cast<void>(
				entities.insertNode(snapshot, PageId(101U), 3U, nodeWithoutProperties, nextCheckpointLsn(*metadata)));
	}
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, roots, pages, catalogPlannerOptions());
	publishPropertyTokens(catalog, planner, {"name"});

	const std::array<IndexDefinition, 1U> definitions{IndexDefinition{17U,
			"node_name",
			IndexDefinitionEntityKind::IDEK_NODE,
			IndexDefinitionKind::IDK_PROPERTY,
			0U,
			{"name"},
			0U,
			""}};
	const OfflineScalarIndexMaterializationResult result = OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions());
	EXPECT_EQ(result.materialized_indexes, 1U);
	EXPECT_EQ(result.indexed_entries, 2U);

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex index(17U, pages, *metadata);
	EXPECT_EQ(index.find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue("Alice"), 1U)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue("Bob"), 2U)),
			  std::optional<uint64_t>(2U));
	EXPECT_EQ(index.scan(snapshot, std::nullopt, 3U).size(), 2U);
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest,
	   SkipsPropertyValuesThatExceedTheScalarIndexKeyCapacity) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	populateNative(destination_io, *metadata, pool);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, roots, pages, catalogPlannerOptions());
	publishPropertyTokens(catalog, planner, {"oversized"});

	MetadataSnapshotPin tokenSnapshot = metadata->openSnapshot();
	const std::optional<uint32_t> token = catalog.findPropertyToken(tokenSnapshot, "oversized");
	ASSERT_TRUE(token.has_value());
	tokenSnapshot.release();
	const std::string oversized(RSS_INDEX_KEY_MAX_BYTES, 'x');
	const std::array propertyRoots{
			PropertyCollectionRoot{2U, Node::typeId, {{*token, PropertyValue(oversized)}}},
	};
	const PropertyCollectionRootStoreOptions rootOptions{PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
	static_cast<void>(test::publishPropertyRoots(entities, roots, *metadata, propertyRoots, rootOptions));

	const std::array<IndexDefinition, 1U> definitions{{
			{18U,
			 "oversized_property",
			 IndexDefinitionEntityKind::IDEK_NODE,
			 IndexDefinitionKind::IDK_PROPERTY,
			 0U,
			 {"oversized"},
			 0U,
			 ""},
	}};
	const OfflineScalarIndexMaterializationResult result = OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions());
	EXPECT_EQ(result.materialized_indexes, 1U);
	EXPECT_EQ(result.indexed_entries, 1U);

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex index(18U, pages, *metadata);
	EXPECT_EQ(index.find(snapshot, IndexKeyCodec::encodeEntryKey(PropertyValue("Alice"), 1U)),
			  std::optional<uint64_t>(1U));
	EXPECT_EQ(index.scan(snapshot, std::nullopt, 2U).size(), 1U);
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest,
	   SkipsDeclaredPropertiesThatAreAbsentBetweenStoredPropertyTokens) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	populateNative(destination_io, *metadata, pool);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	NativeGraphWritePlanner planner(*metadata, entities, labels, roots, pages, catalogPlannerOptions());
	publishPropertyTokens(catalog, planner, {"first", "middle", "last"});

	const std::array propertyRoots{
			PropertyCollectionRoot{1U, Node::typeId, {{1U, PropertyValue("Alice")}, {3U, PropertyValue("A")}}},
			PropertyCollectionRoot{2U, Node::typeId, {{1U, PropertyValue("Bob")}, {3U, PropertyValue("B")}}},
	};
	const PropertyCollectionRootStoreOptions rootOptions{PhysicalSlotId(40U), PhysicalSlotId(59U), 401U};
	static_cast<void>(test::publishPropertyRoots(entities, roots, *metadata, propertyRoots, rootOptions));

	const std::array<IndexDefinition, 1U> definitions{{
			{19U,
			 "declared_but_absent",
			 IndexDefinitionEntityKind::IDEK_NODE,
			 IndexDefinitionKind::IDK_PROPERTY,
			 0U,
			 {"middle"},
			 0U,
			 ""},
	}};
	const OfflineScalarIndexMaterializationResult result = OfflineScalarIndexMaterializer::materialize(
			definitions, pages, entities, labels, roots, catalog, *metadata, indexOptions());
	EXPECT_EQ(result.materialized_indexes, 1U);
	EXPECT_EQ(result.indexed_entries, 0U);

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(BTreeIndex(19U, pages, *metadata).scan(snapshot, std::nullopt, 1U).empty());
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest, RejectsInvalidOptionsBeforeScanningDestinationData) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	OfflineScalarIndexMaterializationOptions invalid = indexOptions();
	invalid.sort.maximum_entries = 127U;
	expectFormatError(
		[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, pages, entities, labels, roots, catalog, *metadata, invalid));
		},
		FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest,
	   RejectsEveryZeroCapacityAndDefinitionLimitBeforeScanningOrPublishing) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);

	for (const auto makeInvalid = [this](auto adjust) {
			OfflineScalarIndexMaterializationOptions options = indexOptions();
			adjust(options);
			return options;
		};
		 const OfflineScalarIndexMaterializationOptions &invalid:
		 {makeInvalid([](auto &options) { options.maximum_definitions = 0U; }),
		  makeInvalid([](auto &options) { options.maximum_property_names = 0U; }),
		  makeInvalid([](auto &options) { options.build.maximum_entries = 0U; }),
		  makeInvalid([](auto &options) { options.build.maximum_pages = 0U; })}) {
		expectFormatError([&] { OfflineScalarIndexMaterializer::validateOptions(invalid); },
					  FormatError::FERR_INVALID_HEADER);
	}

	OfflineScalarIndexMaterializationOptions definitionBound = indexOptions();
	definitionBound.maximum_definitions = 1U;
	const std::array<IndexDefinition, 2U> definitions{{
			{1U, "first", IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_LABEL, 0U, {}, 0U, ""},
			{2U, "second", IndexDefinitionEntityKind::IDEK_EDGE, IndexDefinitionKind::IDK_LABEL, 0U, {}, 0U, ""},
	}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, definitionBound));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin after = metadata->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	after.release();
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest,
	   RejectsEveryDestinationStoreBoundToADifferentMetadataStore) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	PropertyCollectionRootStore roots(pool, *metadata);
	BTreePageStore catalogPages(pool, *metadata);
	SystemCatalog catalog(catalogPages, *metadata);

	// The materializer must not mix a page store from another physical database.
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(wal_io, initialSuperblock());
	SlottedPagePool foreignPool(wal_io, *foreignMetadata);
	BTreePageStore foreignPages(foreignPool, *foreignMetadata);
	EntityPageStore foreignEntities(wal_io, *foreignMetadata);
	EntityLabelSetStore foreignLabels(wal_io, *foreignMetadata, foreignPool);
	PropertyCollectionRootStore foreignRoots(foreignPool, *foreignMetadata);
	SystemCatalog foreignCatalog(foreignPages, *foreignMetadata);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, foreignPages, entities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, catalogPages, foreignEntities, labels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, catalogPages, entities, foreignLabels, roots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, catalogPages, entities, labels, foreignRoots, catalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						{}, catalogPages, entities, labels, roots, foreignCatalog, *metadata, indexOptions()));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableOfflineScalarIndexMaterializerTest,
	   RejectsAnIndexDefinitionWhosePropertyNamesExceedTheConfiguredMaterializationBound) {
	auto metadata = RelocatableMetadataStore::initializeNew(destination_io, initialSuperblock());
	SlottedPagePool pool(destination_io, *metadata);
	EntityPageStore entities(destination_io, *metadata);
	EntityLabelSetStore labels(destination_io, *metadata, pool);
	BTreePageStore pages(pool, *metadata);
	PropertyCollectionRootStore roots(pool, *metadata);
	SystemCatalog catalog(pages, *metadata);
	OfflineScalarIndexMaterializationOptions options = indexOptions();
	options.maximum_property_names = 1U;
	const std::array<IndexDefinition, 1U> definitions{{
			{1U,
			 "composite",
			 IndexDefinitionEntityKind::IDEK_NODE,
			 IndexDefinitionKind::IDK_COMPOSITE,
			 0U,
			 {"first", "second"},
			 0U,
			 ""},
	}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();
	expectFormatError(
			[&] {
				static_cast<void>(OfflineScalarIndexMaterializer::materialize(
						definitions, pages, entities, labels, roots, catalog, *metadata, options));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin after = metadata->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	after.release();
}
