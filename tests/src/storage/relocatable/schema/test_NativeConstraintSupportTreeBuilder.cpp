/**
 * @file test_NativeConstraintSupportTreeBuilder.cpp
 * @brief Canonical System Catalog constraint support-tree planning tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/NativeConstraintDefinitionWriter.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/NativeConstraintSupportTreeBuilder.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x6C6D6E6F70717273ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(90U);
		options.entity_pool_owner_id = 501U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(90U), 502U};
		return options;
	}

	NativeConstraintSupportTreeBuildOptions supportTreeOptions() {
		return {PhysicalSlotId(40U), PhysicalSlotId(90U), 64U, 64U, 2048U, 4U, 8U};
	}

	std::vector<std::byte> supportTreeProbeKey(uint64_t entityId) {
		std::vector<std::byte> key(RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES);
		for (size_t offset = 0U; offset < sizeof(entityId); ++offset) {
			const size_t shift = (sizeof(entityId) - 1U - offset) * 8U;
			key[key.size() - sizeof(entityId) + offset] = static_cast<std::byte>((entityId >> shift) & 0xFFU);
		}
		return key;
	}

	size_t supportTreeEntryCountRequiringMultiplePages() {
		const PageOwnerHeader owner{PageId(1U),
									 RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST,
									 PageOwnerKind::POK_INDEX,
									 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 Generation(1U),
									 LogSequenceNumber(1U)};
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		BTreePage page = BTreePage::create(owner, header);
		for (uint64_t entityId = 1U;; ++entityId) {
			try {
				page.insert({supportTreeProbeKey(entityId), entityId});
			} catch (const FormatException &error) {
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				return static_cast<size_t>(entityId);
			}
			if (entityId == std::numeric_limits<uint64_t>::max()) {
				throw std::runtime_error("B+Tree leaf capacity cannot be exceeded safely");
			}
		}
	}

	NativeConstraintPropertyTokenResolver propertyTokenResolver() {
		return [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			return name == "key" ? std::optional<uint32_t>(1U) : std::nullopt;
		};
	}

	ConstraintDefinition uniqueConstraint() {
		return {0U,
				"native_person_key",
				ConstraintDefinitionEntityKind::CDEK_NODE,
				ConstraintDefinitionKind::CDK_UNIQUE,
				7U,
				{"key"},
				ConstraintDefinitionPropertyType::CDPT_NONE};
	}

	ConstraintDefinition uniqueConstraintForPlanning() {
		ConstraintDefinition result = uniqueConstraint();
		result.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		return result;
	}

	ConstraintDefinition notNullConstraint() {
		ConstraintDefinition result = uniqueConstraint();
		result.name = "native_person_key_required";
		result.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
		return result;
	}

	ConstraintDefinition nodeKeyConstraintForPlanning() {
		ConstraintDefinition result = uniqueConstraint();
		result.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		result.name = "native_person_node_key";
		result.kind = ConstraintDefinitionKind::CDK_NODE_KEY;
		return result;
	}

	ConstraintDefinition nodeKeyConstraint() {
		ConstraintDefinition result = uniqueConstraint();
		result.name = "native_person_key_required";
		result.kind = ConstraintDefinitionKind::CDK_NODE_KEY;
		return result;
	}

	ConstraintDefinition edgeUniqueConstraint() {
		ConstraintDefinition result = uniqueConstraint();
		result.name = "native_edge_key";
		result.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE;
		result.schema_token_id = 8U;
		return result;
	}

	IndexDefinition nativeVectorIndex() {
		return {0U,
				"native_embedding",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_VECTOR,
				7U,
				{"embedding"},
				2U,
				"l2"};
	}

	PropertyCollectionRoot rootFor(uint64_t nodeId, PropertyValue value) {
		return {nodeId, Node::typeId, {{1U, std::move(value)}}};
	}

	PropertyCollectionRoot edgeRootFor(uint64_t edgeId, PropertyValue value) {
		return {edgeId, Edge::typeId, {{1U, std::move(value)}}};
	}

	ConstraintDefinition stageConstraintCreate(
			NativeGraphWriteTransaction &transaction, SystemCatalog &catalog, BTreePageStore &indexes,
			RelocatableMetadataStore &metadata, PageBackedGraphReadAdapter &graph, ConstraintDefinition definition,
			const NativeConstraintSupportTreeBuildOptions &options) {
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		NativeConstraintDefinitionWriter::stageCreate(transaction, catalog, session, indexes, metadata, definition, graph,
				options, propertyTokenResolver());
		session.finish();
		return definition;
	}

	void stageConstraintDrop(NativeGraphWriteTransaction &transaction, SystemCatalog &catalog, BTreePageStore &indexes,
			RelocatableMetadataStore &metadata, const ConstraintDefinition &definition, size_t maximumTreePages) {
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		NativeConstraintDefinitionWriter::stageDrop(transaction, catalog, session, indexes, metadata, definition,
				maximumTreePages);
		session.finish();
	}

} // namespace

class NativeConstraintSupportTreeBuilderTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> slottedPages_;
	std::unique_ptr<EntityLabelSetStore> labels_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("native_constraint_support_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slottedPages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slottedPages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slottedPages_, *metadata_);
	}

	void reopenNativeStores() {
		properties_.reset();
		labels_.reset();
		slottedPages_.reset();
		entities_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slottedPages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slottedPages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slottedPages_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		labels_.reset();
		slottedPages_.reset();
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
};

TEST_F(NativeConstraintSupportTreeBuilderTest, BuildsAndPublishesANativeTreeFromOwnerKeyedProperties) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t outsideNode = create.createNode(std::array<uint64_t, 1U>{9U});
	create.replaceNodeProperty(rootFor(firstNode, PropertyValue("Ada")));
	create.replaceNodeProperty(rootFor(secondNode, PropertyValue("Bert")));
	create.replaceNodeProperty(rootFor(outsideNode, PropertyValue("Outside")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, uniqueConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConstraintDefinitionById(current, definition.constraint_id),
			std::optional<ConstraintDefinition>{definition});
	EXPECT_EQ(catalog.findConstraintDefinitionByName(current, definition.name),
			std::optional<ConstraintDefinition>{definition});
	const std::vector<BTreePageEntry> entries =
			BTreeIndex(definition.constraint_id, indexes, *metadata_).scan(current, std::nullopt, 8U);
	ASSERT_EQ(entries.size(), 2U);
	EXPECT_NE(std::ranges::find(entries, firstNode, &BTreePageEntry::value_or_child), entries.end());
	EXPECT_NE(std::ranges::find(entries, secondNode, &BTreePageEntry::value_or_child), entries.end());
	EXPECT_EQ(std::ranges::find(entries, outsideNode, &BTreePageEntry::value_or_child), entries.end());
	current.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, FirstNativeCreatePublishesCatalogAndSupportTreeAtomically) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	create.replaceNodeProperty(rootFor(nodeId, PropertyValue("Ada")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ASSERT_FALSE(metadata_->findIndexRootOptional(publish.baseSnapshot(), RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, uniqueConstraint(), supportTreeOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();
	EXPECT_FALSE(catalog.findConstraintDefinitionByName(before, definition.name).has_value());
	ASSERT_TRUE(publish.commit().committed());
	EXPECT_FALSE(catalog.findConstraintDefinitionByName(before, definition.name).has_value());
	before.release();

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, definition.constraint_id).has_value());
	EXPECT_EQ(catalog.findConstraintDefinitionByName(snapshot, definition.name),
			std::optional<ConstraintDefinition>{definition});
	snapshot.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, IndexDefinitionsUseTheSameCatalogRootAndAtomicDirections) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(publish);
	IndexDefinition definition = session.createIndexDefinition(nativeVectorIndex());
	session.finish();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	EXPECT_FALSE(catalog.findIndexDefinitionByName(before, definition.name).has_value());
	ASSERT_TRUE(publish.commit().committed());
	EXPECT_FALSE(catalog.findIndexDefinitionByName(before, definition.name).has_value());
	before.release();

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, definition.index_id), std::optional<IndexDefinition>{definition});
	EXPECT_EQ(catalog.findIndexDefinitionByName(snapshot, definition.name), std::optional<IndexDefinition>{definition});
	EXPECT_EQ(catalog.findIndexDefinitionBySchema(snapshot, definition.entity_kind, definition.kind,
			definition.label_token_id, definition.property_names, definition.vector_dimensions, definition.vector_metric),
			std::optional<IndexDefinition>{definition});
	snapshot.release();

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	SystemCatalogWriteSession dropSession = catalog.beginWrite(drop);
	dropSession.dropIndexDefinition(definition);
	dropSession.finish();
	ASSERT_TRUE(drop.commit().committed());
	MetadataSnapshotPin afterDrop = metadata_->openSnapshot();
	EXPECT_FALSE(catalog.findIndexDefinitionByName(afterDrop, definition.name).has_value());
	afterDrop.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, DropsCatalogAndSupportTreeInOneSnapshotGeneration) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	create.replaceNodeProperty(rootFor(nodeId, PropertyValue("Ada")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, uniqueConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin beforeDrop = metadata_->openSnapshot();
	const IndexRootLocation originalRoot = metadata_->findIndexRoot(beforeDrop, definition.constraint_id);
	NativeGraphWriteTransaction drop = planner.beginTransaction();
	stageConstraintDrop(drop, catalog, indexes, *metadata_, definition, supportTreeOptions().maximum_pages);
	EXPECT_EQ(metadata_->findIndexRoot(drop.baseSnapshot(), definition.constraint_id), originalRoot);
	ASSERT_TRUE(drop.commit().committed());

	EXPECT_EQ(metadata_->findIndexRoot(beforeDrop, definition.constraint_id), originalRoot);
	EXPECT_EQ(catalog.findConstraintDefinitionByName(beforeDrop, definition.name),
			std::optional<ConstraintDefinition>{definition});
	EXPECT_EQ(BTreeIndex(definition.constraint_id, indexes, *metadata_).scan(beforeDrop, std::nullopt, 8U).size(), 1U);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, definition.constraint_id).has_value());
	EXPECT_FALSE(catalog.findConstraintDefinitionById(current, definition.constraint_id).has_value());
	EXPECT_FALSE(catalog.findConstraintDefinitionByName(current, definition.name).has_value());
	EXPECT_THROW(static_cast<void>(metadata_->findPageLocation(current, originalRoot.root_page_id)), FormatException);
	current.release();
	beforeDrop.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, PublishesAndDropsNotNullConstraintWithoutSupportTree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);

	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, notNullConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin created = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConstraintDefinitionById(created, definition.constraint_id),
			std::optional<ConstraintDefinition>{definition});
	EXPECT_FALSE(metadata_->findIndexRootOptional(created, definition.constraint_id).has_value());
	created.release();

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	stageConstraintDrop(drop, catalog, indexes, *metadata_, definition, supportTreeOptions().maximum_pages);
	ASSERT_TRUE(drop.commit().committed());

	MetadataSnapshotPin dropped = metadata_->openSnapshot();
	EXPECT_FALSE(catalog.findConstraintDefinitionById(dropped, definition.constraint_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(dropped, definition.constraint_id).has_value());
	dropped.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, PublishesAndDropsNodeKeyWithSupportTree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	create.replaceNodeProperty(rootFor(nodeId, PropertyValue("Ada")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, nodeKeyConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin created = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findIndexRootOptional(created, definition.constraint_id).has_value());
	created.release();

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	stageConstraintDrop(drop, catalog, indexes, *metadata_, definition, supportTreeOptions().maximum_pages);
	ASSERT_TRUE(drop.commit().committed());

	MetadataSnapshotPin dropped = metadata_->openSnapshot();
	EXPECT_FALSE(catalog.findConstraintDefinitionById(dropped, definition.constraint_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(dropped, definition.constraint_id).has_value());
	dropped.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsDroppingNotNullConstraintWithUnexpectedSupportTree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);

	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, notNullConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	const BTreePageEntry entry{supportTreeProbeKey(1U), 1U};
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexCreate(indexes, definition.constraint_id, std::span<const BTreePageEntry>(&entry, 1U));
	ASSERT_TRUE(corrupt.commit().committed());

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(drop);
	EXPECT_THROW(NativeConstraintDefinitionWriter::stageDrop(drop, catalog, session, indexes, *metadata_, definition,
			supportTreeOptions().maximum_pages), FormatException);
	drop.rollback();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConstraintDefinitionById(current, definition.constraint_id),
			std::optional<ConstraintDefinition>{definition});
	EXPECT_TRUE(metadata_->findIndexRootOptional(current, definition.constraint_id).has_value());
	current.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsDroppingUniqueConstraintWithoutSupportTree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	NativeGraphWriteTransaction publish = planner.beginTransaction();
	SystemCatalogWriteSession publishSession = catalog.beginWrite(publish);
	ConstraintDefinition definition = publishSession.createConstraintDefinition(uniqueConstraint());
	publishSession.finish();
	ASSERT_TRUE(publish.commit().committed());

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	SystemCatalogWriteSession dropSession = catalog.beginWrite(drop);
	EXPECT_THROW(NativeConstraintDefinitionWriter::stageDrop(drop, catalog, dropSession, indexes, *metadata_, definition,
			supportTreeOptions().maximum_pages), FormatException);
	drop.rollback();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConstraintDefinitionById(current, definition.constraint_id),
			std::optional<ConstraintDefinition>{definition});
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, definition.constraint_id).has_value());
	current.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsForeignMetadataDomainsBeforeStagingConstraintDdl) {
	const auto uuid = boost::uuids::random_generator()();
	const std::filesystem::path foreignPath =
			std::filesystem::temp_directory_path() / ("native_constraint_foreign_" + to_string(uuid));
	{
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		std::unique_ptr<RelocatableMetadataStore> foreignMetadata =
				RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		SlottedPagePool foreignSlottedPages(foreignIo, *foreignMetadata);
		BTreePageStore foreignIndexes(foreignSlottedPages, *foreignMetadata);
		SystemCatalog foreignCatalog(foreignIndexes, *foreignMetadata);

		BTreePageStore indexes(*slottedPages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);

		ConstraintDefinition definition = notNullConstraint();
		EXPECT_THROW(NativeConstraintDefinitionWriter::stageCreate(transaction, catalog, session, foreignIndexes, *metadata_,
				definition, graph, supportTreeOptions(), propertyTokenResolver()), FormatException);
		EXPECT_THROW(NativeConstraintDefinitionWriter::stageCreate(transaction, foreignCatalog, session, indexes, *metadata_,
				definition, graph, supportTreeOptions(), propertyTokenResolver()), FormatException);
		EXPECT_THROW(NativeConstraintDefinitionWriter::stageDrop(transaction, catalog, session, foreignIndexes, *metadata_,
				definition, supportTreeOptions().maximum_pages), FormatException);
		EXPECT_THROW(NativeConstraintDefinitionWriter::stageDrop(transaction, foreignCatalog, session, indexes, *metadata_,
				definition, supportTreeOptions().maximum_pages), FormatException);
		transaction.rollback();
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RecoversNativeConstraintDropAsOneAtomicGeneration) {
	ConstraintDefinition definition;
	{
		BTreePageStore indexes(*slottedPages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		NativeGraphWriteTransaction create = planner.beginTransaction();
		const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
		create.replaceNodeProperty(rootFor(nodeId, PropertyValue("Ada")));
		ASSERT_TRUE(create.commit().committed());
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
		NativeGraphWriteTransaction publish = planner.beginTransaction();
		definition = stageConstraintCreate(publish, catalog, indexes, *metadata_, graph, uniqueConstraint(), supportTreeOptions());
		ASSERT_TRUE(publish.commit().committed());

		NativeGraphWriteTransaction interruptedDrop = planner.beginTransaction();
		stageConstraintDrop(interruptedDrop, catalog, indexes, *metadata_, definition, supportTreeOptions().maximum_pages);
		MetadataPublishOptions beforePublication;
		beforePublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interruptedDrop.commit(beforePublication)), BootstrapPublishInterrupted);
		EXPECT_EQ(interruptedDrop.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}

	reopenNativeStores();
	{
		BTreePageStore indexes(*slottedPages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		MetadataSnapshotPin retained = metadata_->openSnapshot();
		ASSERT_TRUE(metadata_->findIndexRootOptional(retained, definition.constraint_id).has_value());
		EXPECT_EQ(catalog.findConstraintDefinitionById(retained, definition.constraint_id),
			std::optional<ConstraintDefinition>{definition});
		retained.release();

		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		NativeGraphWriteTransaction interruptedDrop = planner.beginTransaction();
		stageConstraintDrop(interruptedDrop, catalog, indexes, *metadata_, definition, supportTreeOptions().maximum_pages);
		MetadataPublishOptions afterPublication;
		afterPublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interruptedDrop.commit(afterPublication)), BootstrapPublishInterrupted);
		EXPECT_EQ(interruptedDrop.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}

	reopenNativeStores();
	{
		BTreePageStore indexes(*slottedPages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		MetadataSnapshotPin dropped = metadata_->openSnapshot();
		EXPECT_FALSE(metadata_->findIndexRootOptional(dropped, definition.constraint_id).has_value());
		EXPECT_FALSE(catalog.findConstraintDefinitionById(dropped, definition.constraint_id).has_value());
		EXPECT_FALSE(catalog.findConstraintDefinitionByName(dropped, definition.name).has_value());
		dropped.release();
	}
}

TEST_F(NativeConstraintSupportTreeBuilderTest, DropsEveryReachablePageOfAMultiPageSupportTree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const size_t entryCount = supportTreeEntryCountRequiringMultiplePages();
	for (uint64_t ordinal = 0U; ordinal < entryCount; ++ordinal) {
		const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
		create.replaceNodeProperty(rootFor(nodeId, PropertyValue(std::string(120U, 'x') + std::to_string(ordinal))));
	}
	ASSERT_TRUE(create.commit().committed());

	NativeConstraintSupportTreeBuildOptions options = supportTreeOptions();
	options.maximum_entries = entryCount;
	options.sort_run_bytes = 8192U;
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(publish, catalog, indexes, *metadata_, graph, uniqueConstraint(), options);
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin beforeDrop = metadata_->openSnapshot();
	const IndexRootLocation root = metadata_->findIndexRoot(beforeDrop, definition.constraint_id);
	std::vector<PageId> reachable{root.root_page_id};
	for (size_t ordinal = 0U; ordinal < reachable.size(); ++ordinal) {
		const BTreePage page = indexes.read(beforeDrop, reachable[ordinal]);
		if (page.header().kind != BTreeNodeKind::BPN_INTERNAL) {
			continue;
		}
		reachable.push_back(page.header().leftmost_child);
		for (const BTreePageEntry &entry: page.entries()) {
			reachable.emplace_back(entry.value_or_child);
		}
	}
	std::ranges::sort(reachable);
	ASSERT_EQ(std::ranges::adjacent_find(reachable), reachable.end());
	ASSERT_GT(reachable.size(), 1U);

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	stageConstraintDrop(drop, catalog, indexes, *metadata_, definition, options.maximum_pages);
	ASSERT_TRUE(drop.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	for (const PageId pageId: reachable) {
		EXPECT_THROW(static_cast<void>(metadata_->findPageLocation(current, pageId)), FormatException);
	}
	current.release();
	beforeDrop.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsAnIncompleteNativeNodeKeyBeforeItCanReserveATree) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	static_cast<void>(create.createNode(std::array<uint64_t, 1U>{7U}));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	const ConstraintDefinition definition = nodeKeyConstraintForPlanning();
	try {
		static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
				indexes, *metadata_, publish.baseSnapshot(), definition, graph, supportTreeOptions(), propertyTokenResolver()));
		FAIL() << "Expected an incomplete NODE KEY to fail before native tree allocation";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
	}
	publish.rollback();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, definition.constraint_id).has_value());
	current.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsInvalidPublicPlanningContractsBeforeGraphAccess) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const ConstraintDefinition valid = uniqueConstraintForPlanning();

	NativeConstraintSupportTreeBuildOptions invalidOptions = supportTreeOptions();
	invalidOptions.maximum_entries = 0U;
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, snapshot, valid, graph, invalidOptions, propertyTokenResolver())), FormatException);

	ConstraintDefinition notNull = valid;
	notNull.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, snapshot, notNull, graph, supportTreeOptions(), propertyTokenResolver())), FormatException);

	ConstraintDefinition nonCatalogId = valid;
	nonCatalogId.constraint_id = 1U;
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, snapshot, nonCatalogId, graph, supportTreeOptions(), propertyTokenResolver())), FormatException);
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, snapshot, valid, graph, supportTreeOptions(), {})), FormatException);
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(indexes, *metadata_, snapshot, valid, graph,
		supportTreeOptions(), [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> {
			return 0U;
		})), FormatException);
	snapshot.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsSupportTreePlanningAcrossMetadataDomains) {
	const auto uuid = boost::uuids::random_generator()();
	const std::filesystem::path foreignPath =
			std::filesystem::temp_directory_path() / ("native_constraint_planning_foreign_" + to_string(uuid));
	{
		auto foreignFile = std::make_shared<std::fstream>(
				foreignPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(foreignFile->is_open());
		auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		std::unique_ptr<RelocatableMetadataStore> foreignMetadata =
				RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
		SlottedPagePool foreignSlottedPages(foreignIo, *foreignMetadata);
		BTreePageStore foreignIndexes(foreignSlottedPages, *foreignMetadata);

		BTreePageStore indexes(*slottedPages_, *metadata_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(foreignIndexes, *metadata_, snapshot,
				uniqueConstraintForPlanning(), graph, supportTreeOptions(), propertyTokenResolver())), FormatException);
		snapshot.release();
	}
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsReplacingAnExistingSupportTreeRoot) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	const ConstraintDefinition definition = uniqueConstraintForPlanning();

	NativeGraphWriteTransaction publish = planner.beginTransaction();
	BTreeInitialTreeMutationPlan initial = NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, publish.baseSnapshot(), definition, graph, supportTreeOptions(), propertyTokenResolver());
	publish.stageScalarIndexCreate(indexes, std::move(initial));
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
			indexes, *metadata_, current, definition, graph, supportTreeOptions(), propertyTokenResolver())), FormatException);
	current.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, SkipsNullableUniqueValuesButRejectsNullNodeKeyValues) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nullNodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	create.replaceNodeProperty(rootFor(nullNodeId, PropertyValue()));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_NO_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(indexes, *metadata_, snapshot,
		uniqueConstraintForPlanning(), graph, supportTreeOptions(), propertyTokenResolver())));
	EXPECT_THROW(static_cast<void>(NativeConstraintSupportTreeBuilder::plan(indexes, *metadata_, snapshot,
		nodeKeyConstraintForPlanning(), graph, supportTreeOptions(), propertyTokenResolver())), FormatException);
	snapshot.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, BuildsAnEdgeScopedTreeFromNativeProperties) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edge = create.createEdge(source, target, 8U);
	create.replaceEdgeProperty(edgeRootFor(edge, PropertyValue("calls")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = stageConstraintCreate(
			publish, catalog, indexes, *metadata_, graph, edgeUniqueConstraint(), supportTreeOptions());
	ASSERT_TRUE(publish.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::vector<BTreePageEntry> entries =
			BTreeIndex(definition.constraint_id, indexes, *metadata_).scan(snapshot, std::nullopt, 8U);
	ASSERT_EQ(entries.size(), 1U);
	EXPECT_EQ(entries.front().value_or_child, edge);
	EXPECT_EQ(catalog.findConstraintDefinitionBySchema(snapshot, definition.entity_kind, definition.kind,
			definition.schema_token_id, definition.property_names, definition.property_type),
			std::optional<ConstraintDefinition>{definition});
	snapshot.release();
}

TEST_F(NativeConstraintSupportTreeBuilderTest, RejectsDuplicateCanonicalValuesBeforePublication) {
	BTreePageStore indexes(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = create.createNode(std::array<uint64_t, 1U>{7U});
	create.replaceNodeProperty(rootFor(firstNode, PropertyValue("same")));
	create.replaceNodeProperty(rootFor(secondNode, PropertyValue("same")));
	ASSERT_TRUE(create.commit().committed());

	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, indexes);
	NativeGraphWriteTransaction publish = planner.beginTransaction();
	ConstraintDefinition definition = uniqueConstraint();
	definition.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
	try {
		static_cast<void>(NativeConstraintSupportTreeBuilder::plan(
				indexes, *metadata_, publish.baseSnapshot(), definition, graph, supportTreeOptions(), propertyTokenResolver()));
		FAIL() << "Expected duplicate canonical values to be rejected";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
	}
	publish.rollback();

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, definition.constraint_id).has_value());
	snapshot.release();
}
