/**
 * @file test_NativeBulkEdgeCatalogTokenMaterializer.cpp
 * @brief Bounded streamed Edge Catalog-token materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/NativeBulkEdgeCatalogTokenMaterializer.hpp"

using namespace graph::storage;

namespace {

	[[nodiscard]] Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0xABCD123400001234ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	[[nodiscard]] NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(160U);
		options.entity_pool_owner_id = 811U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(160U), 812U};
		return options;
	}

	[[nodiscard]] OrderedBTreeEntrySource tokenSource(std::vector<BTreePageEntry> entries) {
		return [entries = std::move(entries)] {
			return [entries, ordinal = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
				if (ordinal == entries.size()) {
					return std::nullopt;
				}
				return entries[ordinal++];
			};
		};
	}

	[[nodiscard]] std::vector<NativeBulkEdgeCatalogTokenResolution>
	readResolutions(const NativeBulkEdgeCatalogTokenResolutionSource &source) {
		std::vector<NativeBulkEdgeCatalogTokenResolution> result;
		NativeBulkEdgeCatalogTokenResolutionCursor cursor = source();
		while (const std::optional<NativeBulkEdgeCatalogTokenResolution> value = cursor()) {
			result.push_back(*value);
		}
		return result;
	}

	[[nodiscard]] std::map<std::vector<std::byte>, uint64_t> readEntries(const OrderedBTreeEntrySource &source) {
		std::map<std::vector<std::byte>, uint64_t> result;
		OrderedBTreeEntryCursor cursor = source();
		std::optional<std::vector<std::byte>> previous;
		while (const std::optional<BTreePageEntry> entry = cursor()) {
			if (previous.has_value()) {
				EXPECT_TRUE(std::lexicographical_compare(previous->begin(), previous->end(), entry->key.begin(),
														 entry->key.end()));
			}
			previous = entry->key;
			EXPECT_TRUE(result.emplace(entry->key, entry->value_or_child).second);
		}
		return result;
	}

} // namespace

class NativeBulkEdgeCatalogTokenMaterializerTest : public ::testing::Test {
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
		path_ = std::filesystem::temp_directory_path() /
				("native_bulk_catalog_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
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

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest,
	   ResolvesExistingTokensAllocatesMissingTokensAndBuildsARewindableOverlay) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(seed);
	EXPECT_EQ(session.ensurePropertyToken("existing_property"), 1U);
	EXPECT_EQ(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "EXISTING_TYPE"), 1U);
	session.finish();
	ASSERT_TRUE(seed.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::vector<BTreePageEntry> tokens = {
			{NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "existing_property"),
			 0U},
			{NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "new_property"), 0U},
			{NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE,
													  "EXISTING_TYPE"),
			 0U},
			{NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "NEW_TYPE"),
			 0U}};
	const NativeBulkEdgeCatalogTokenMaterialization materialization = NativeBulkEdgeCatalogTokenMaterializer::prepare(
			catalog, snapshot, tokenSource(tokens), {tokens.size(), 64U});
	EXPECT_TRUE(materialization.catalog_changed);
	EXPECT_EQ(materialization.new_property_tokens, 1U);
	EXPECT_EQ(materialization.new_schema_tokens, 1U);
	const std::vector<NativeBulkEdgeCatalogTokenResolution> expected = {
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "existing_property", 1U, false},
			{NativeBulkEdgeTokenKind::NBETK_PROPERTY, "new_property", 2U, true},
			{NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "EXISTING_TYPE", 1U, false},
			{NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "NEW_TYPE", 2U, true}};
	EXPECT_EQ(readResolutions(materialization.resolutions), expected);
	EXPECT_EQ(readResolutions(materialization.resolutions), expected);

	const std::map<std::vector<std::byte>, uint64_t> overlay = readEntries(materialization.final_catalog_entries);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::bootstrapKey()), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyNameKey("existing_property")), 1U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyNameKey("new_property")), 2U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyTokenKey(2U, "new_property")), 2U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaNameKey(
					  SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "NEW_TYPE")),
			  2U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaTokenKey(
					  SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 2U, "NEW_TYPE")),
			  2U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyTokenSequenceKey()), 3U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaTokenSequenceKey()), 3U);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, RejectsUnorderedOrOverBoundTokenSourcesBeforeAnyPublication) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry later{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL"), 0U};
	const BTreePageEntry earlier{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property"), 0U};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
						 catalog, snapshot, tokenSource({later, earlier}), {2U, 64U})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot,
																				   tokenSource({earlier}), {0U, 64U})),
				 FormatException);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, LeavesTheCatalogAbsentForAnEmptyTokenStream) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeCatalogTokenMaterialization materialization =
			NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot, tokenSource({}), {1U, 64U});
	EXPECT_FALSE(materialization.catalog_changed);
	EXPECT_TRUE(readResolutions(materialization.resolutions).empty());
	EXPECT_TRUE(readEntries(materialization.final_catalog_entries).empty());
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, UpdatesExistingGraphCardinalityInTheSameRewindableCatalogOverlay) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(seed);
	session.setGraphCardinalityStatistics({2U, 3U});
	session.finish();
	ASSERT_TRUE(seed.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeCatalogTokenMaterialization materialization =
			NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot, tokenSource({}), {1U, 64U}, 4U);
	EXPECT_TRUE(materialization.graph_cardinality_updated);
	EXPECT_TRUE(materialization.catalog_changed);
	EXPECT_TRUE(readResolutions(materialization.resolutions).empty());
	const std::map<std::vector<std::byte>, uint64_t> overlay = readEntries(materialization.final_catalog_entries);
	EXPECT_EQ(overlay.at(SystemCatalogGraphCardinalityCodec::nodeCountKey()),
			  SystemCatalogGraphCardinalityCodec::encodeCount(2U));
	EXPECT_EQ(overlay.at(SystemCatalogGraphCardinalityCodec::edgeCountKey()),
			  SystemCatalogGraphCardinalityCodec::encodeCount(7U));
	snapshot.release();
}
