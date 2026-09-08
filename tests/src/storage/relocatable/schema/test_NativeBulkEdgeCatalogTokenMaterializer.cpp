/**
 * @file test_NativeBulkEdgeCatalogTokenMaterializer.cpp
 * @brief Bounded streamed Edge Catalog-token materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <algorithm>
#include <array>
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

TEST(SystemCatalogTokenKeyCodecTest, RejectsInvalidSchemaKindsAndNamesAtThePublicCodecBoundary) {
	EXPECT_NO_THROW(SystemCatalogTokenKeyCodec::validateSchemaKind(SystemCatalogSchemaTokenKind::SCSTK_LABEL));
	EXPECT_NO_THROW(SystemCatalogTokenKeyCodec::validateSchemaKind(
			SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE));
	EXPECT_THROW(SystemCatalogTokenKeyCodec::validateSchemaKind(static_cast<SystemCatalogSchemaTokenKind>(0U)),
			 FormatException);

	EXPECT_NO_THROW(SystemCatalogTokenKeyCodec::validateName("catalog_name"));
	EXPECT_THROW(SystemCatalogTokenKeyCodec::validateName(""), FormatException);
	const std::string oversized(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, 'x');
	EXPECT_THROW(SystemCatalogTokenKeyCodec::validateName(oversized), FormatException);
}

TEST(SystemCatalogTokenKeyCodecTest, ProducesStableKeysInSeparateTokenNamespaces) {
	const std::vector<std::byte> bootstrap = SystemCatalogTokenKeyCodec::bootstrapKey();
	const std::vector<std::byte> propertyName = SystemCatalogTokenKeyCodec::propertyNameKey("name");
	const std::vector<std::byte> propertyToken = SystemCatalogTokenKeyCodec::propertyTokenKey(7U, "name");
	const std::vector<std::byte> labelName =
			SystemCatalogTokenKeyCodec::schemaNameKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "name");
	const std::vector<std::byte> relationshipName = SystemCatalogTokenKeyCodec::schemaNameKey(
			SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "name");
	const std::vector<std::byte> labelToken =
			SystemCatalogTokenKeyCodec::schemaTokenKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, 7U, "name");
	const std::vector<std::byte> relationshipToken = SystemCatalogTokenKeyCodec::schemaTokenKey(
			SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7U, "name");
	const std::vector<std::byte> propertySequence = SystemCatalogTokenKeyCodec::propertyTokenSequenceKey();
	const std::vector<std::byte> schemaSequence = SystemCatalogTokenKeyCodec::schemaTokenSequenceKey();

	EXPECT_FALSE(bootstrap.empty());
	EXPECT_EQ(propertyName, SystemCatalogTokenKeyCodec::propertyNameKey("name"));
	EXPECT_NE(propertyName, SystemCatalogTokenKeyCodec::propertyNameKey("other"));
	EXPECT_NE(propertyName, propertyToken);
	EXPECT_NE(propertyName, labelName);
	EXPECT_NE(labelName, relationshipName);
	EXPECT_NE(labelToken, relationshipToken);
	EXPECT_NE(bootstrap, propertySequence);
	EXPECT_NE(propertySequence, schemaSequence);
}

TEST(SystemCatalogGraphCardinalityCodecTest, PreservesZeroCountsAndRejectsInvalidPersistedValues) {
	EXPECT_FALSE(SystemCatalogGraphCardinalityCodec::nodeCountKey().empty());
	EXPECT_FALSE(SystemCatalogGraphCardinalityCodec::edgeCountKey().empty());
	EXPECT_EQ(SystemCatalogGraphCardinalityCodec::encodeCount(0U), 1U);
	EXPECT_EQ(SystemCatalogGraphCardinalityCodec::encodeCount(41U), 42U);
	EXPECT_EQ(SystemCatalogGraphCardinalityCodec::decodeCount(1U), 0U);
	EXPECT_EQ(SystemCatalogGraphCardinalityCodec::decodeCount(42U), 41U);
	EXPECT_THROW(static_cast<void>(SystemCatalogGraphCardinalityCodec::encodeCount(std::numeric_limits<uint64_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(SystemCatalogGraphCardinalityCodec::decodeCount(0U)), FormatException);
}

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

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, RejectsIndependentTokenAndCatalogBoundsBeforePublication) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry property{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "property"), 0U};
	const BTreePageEntry relationship{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL"), 0U};
	EXPECT_TRUE(std::lexicographical_compare(property.key.begin(), property.key.end(), relationship.key.begin(),
															 relationship.key.end()));

	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					 catalog, snapshot, tokenSource({property, relationship}), {1U, 64U})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					 catalog, snapshot, tokenSource({property}), {1U, 0U})),
			 FormatException);
	snapshot.release();
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					 catalog, snapshot, tokenSource({property}), {1U, 64U})),
			 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, metadata_->openSnapshot(),
					 OrderedBTreeEntrySource{}, {1U, 64U})),
			 FormatException);
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

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, BuildsAnInitialCatalogForRelationshipTypesWithoutPropertyTokens) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry relationship{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "REL"), 0U};
	const NativeBulkEdgeCatalogTokenMaterialization materialization = NativeBulkEdgeCatalogTokenMaterializer::prepare(
			catalog, snapshot, tokenSource({relationship}), {1U, 64U});

	EXPECT_TRUE(materialization.catalog_changed);
	EXPECT_EQ(materialization.new_property_tokens, 0U);
	EXPECT_EQ(materialization.new_schema_tokens, 1U);
	const std::map<std::vector<std::byte>, uint64_t> overlay = readEntries(materialization.final_catalog_entries);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::bootstrapKey()), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaNameKey(
					  SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "REL")),
			  1U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaTokenSequenceKey()), 2U);
	EXPECT_EQ(overlay.count(SystemCatalogTokenKeyCodec::propertyTokenSequenceKey()), 0U);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, RejectsMalformedTokenEntriesAtResolutionTime) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const auto expectMalformed = [&](BTreePageEntry malformed) {
		EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					catalog, snapshot, tokenSource({std::move(malformed)}), {1U, 64U})),
				 FormatException);
	};

	// Token entries are key-only and must carry a supported kind plus a valid name.
	expectMalformed({NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "p"), 1U});
	expectMalformed({{std::byte{0xffU}, std::byte{'p'}}, 0U});
	expectMalformed({{std::byte{static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_PROPERTY)}}, 0U});
	std::vector<std::byte> oversizedName{
			static_cast<std::byte>(static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_PROPERTY))};
	oversizedName.resize(1U + RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, std::byte{'x'});
	expectMalformed({std::move(oversizedName), 0U});

	const OrderedBTreeEntrySource noCursor = [] { return OrderedBTreeEntryCursor{}; };
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot, noCursor,
																									 {1U, 64U})),
				 FormatException);
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

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest,
	   PreservesAnExistingCatalogForAnEmptyTokenStreamWithoutAStatisticsUpdate) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(seed);
	EXPECT_EQ(session.ensurePropertyToken("persisted"), 1U);
	session.setGraphCardinalityStatistics({2U, 3U});
	session.finish();
	ASSERT_TRUE(seed.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeCatalogTokenMaterialization materialization =
			NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot, tokenSource({}), {1U, 64U});
	EXPECT_FALSE(materialization.graph_cardinality_updated);
	EXPECT_FALSE(materialization.catalog_changed);
	EXPECT_TRUE(readResolutions(materialization.resolutions).empty());
	const std::map<std::vector<std::byte>, uint64_t> overlay = readEntries(materialization.final_catalog_entries);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::bootstrapKey()), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyNameKey("persisted")), 1U);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::propertyTokenSequenceKey()), 2U);
	EXPECT_EQ(overlay.at(SystemCatalogGraphCardinalityCodec::nodeCountKey()),
			  SystemCatalogGraphCardinalityCodec::encodeCount(2U));
	EXPECT_EQ(overlay.at(SystemCatalogGraphCardinalityCodec::edgeCountKey()),
			  SystemCatalogGraphCardinalityCodec::encodeCount(3U));
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, RejectsAnOverflowingIncrementForExistingGraphCardinality) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(seed);
	session.setGraphCardinalityStatistics({2U, std::numeric_limits<uint64_t>::max() - 1U});
	session.finish();
	ASSERT_TRUE(seed.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
				 catalog, snapshot, tokenSource({}), {1U, 64U}, 2U)),
			 FormatException);
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(snapshot),
			  std::optional<NativeGraphCardinalityStatistics>({2U, std::numeric_limits<uint64_t>::max() - 1U}));
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, IgnoresCardinalityIncrementUntilStatisticsExist) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeCatalogTokenMaterialization materialization =
			NativeBulkEdgeCatalogTokenMaterializer::prepare(catalog, snapshot, tokenSource({}), {1U, 64U}, 3U);
	EXPECT_FALSE(materialization.graph_cardinality_updated);
	EXPECT_FALSE(materialization.catalog_changed);
	EXPECT_TRUE(readEntries(materialization.final_catalog_entries).empty());
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest,
	   RejectsAnOutOfRangePersistedPropertySequenceBeforeEmittingAResolution) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 2U> entries = {
			BTreePageEntry{SystemCatalogTokenKeyCodec::bootstrapKey(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{SystemCatalogTokenKeyCodec::propertyTokenSequenceKey(),
							static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U},
	};
	seed.stageScalarIndexCreate(pages, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry property{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "overflow"), 0U};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					 catalog, snapshot, tokenSource({property}), {1U, 64U})),
			 FormatException);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest,
	   AllocatesTheLastRelationshipTokenAndPersistsTheExhaustedSequenceMarker) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 2U> entries = {
			BTreePageEntry{SystemCatalogTokenKeyCodec::bootstrapKey(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{SystemCatalogTokenKeyCodec::schemaTokenSequenceKey(),
							std::numeric_limits<uint64_t>::max()},
	};
	seed.stageScalarIndexCreate(pages, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry relationship{NativeBulkEdgeTokenRunBuilder::encodeKey(
											NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "LAST"),
											0U};
	const NativeBulkEdgeCatalogTokenMaterialization materialization = NativeBulkEdgeCatalogTokenMaterializer::prepare(
			catalog, snapshot, tokenSource({relationship}), {1U, 64U});
	EXPECT_EQ(readResolutions(materialization.resolutions),
			  (std::vector<NativeBulkEdgeCatalogTokenResolution>{
					  {NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "LAST",
					   std::numeric_limits<uint64_t>::max(), true}}));
	const std::map<std::vector<std::byte>, uint64_t> overlay = readEntries(materialization.final_catalog_entries);
	EXPECT_EQ(overlay.at(SystemCatalogTokenKeyCodec::schemaTokenSequenceKey()), 0U);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, RejectsThePersistedExhaustedRelationshipSequenceMarker) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, pages, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 2U> entries = {
			BTreePageEntry{SystemCatalogTokenKeyCodec::bootstrapKey(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{SystemCatalogTokenKeyCodec::schemaTokenSequenceKey(),
							std::numeric_limits<uint64_t>::max()},
	};
	seed.stageScalarIndexCreate(pages, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(pages, *metadata_);
	NativeGraphWriteTransaction exhaust = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(exhaust);
	EXPECT_EQ(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "LAST"),
			  std::numeric_limits<uint64_t>::max());
	session.finish();
	ASSERT_TRUE(exhaust.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry relationship{NativeBulkEdgeTokenRunBuilder::encodeKey(
											NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE, "EXHAUSTED"),
											0U};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgeCatalogTokenMaterializer::prepare(
					 catalog, snapshot, tokenSource({relationship}), {1U, 64U})),
			 FormatException);
	snapshot.release();
}

TEST_F(NativeBulkEdgeCatalogTokenMaterializerTest, ResolutionAndOverlaySourcesDoNotOutliveTheirPlanningSnapshot) {
	BTreePageStore pages(*slottedPages_, *metadata_);
	SystemCatalog catalog(pages, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const BTreePageEntry property{
			NativeBulkEdgeTokenRunBuilder::encodeKey(NativeBulkEdgeTokenKind::NBETK_PROPERTY, "scoped"), 0U};
	const NativeBulkEdgeCatalogTokenMaterialization materialization = NativeBulkEdgeCatalogTokenMaterializer::prepare(
			catalog, snapshot, tokenSource({property}), {1U, 64U});
	snapshot.release();

	// Both exposed sources capture the planning snapshot. They must reject use
	// after its release instead of resolving tokens against a later generation.
	EXPECT_THROW(static_cast<void>(readResolutions(materialization.resolutions)), FormatException);
	EXPECT_THROW(static_cast<void>(readEntries(materialization.final_catalog_entries)), FormatException);
}
