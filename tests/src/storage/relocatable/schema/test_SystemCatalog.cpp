/**
 * @file test_SystemCatalog.cpp
 * @brief Atomic canonical System Catalog tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <stdexcept>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace graph::storage;
using graph::Node;
using graph::PropertyValue;

namespace {


	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 821U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 822U};
		return options;
	}

	std::vector<std::byte> propertyNameKey(std::string_view name) {
		std::vector<std::byte> key = {std::byte{1U}};
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	std::vector<std::byte> propertyTokenKey(uint32_t token, std::string_view name) {
		std::vector<std::byte> key = {std::byte{2U}, static_cast<std::byte>(token >> 24U),
								  static_cast<std::byte>(token >> 16U), static_cast<std::byte>(token >> 8U),
								  static_cast<std::byte>(token)};
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	void appendBigEndianU32(std::vector<std::byte> &target, uint32_t value);

	std::vector<std::byte> configurationPrefix(std::string_view name) {
		std::vector<std::byte> key = {std::byte{12U}};
		appendBigEndianU32(key, static_cast<uint32_t>(name.size()));
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	std::vector<std::byte> configurationKey(std::string_view name, const PropertyValue &value) {
		std::vector<std::byte> key = configurationPrefix(name);
		CanonicalPropertyValueCodec::append(key, value);
		return key;
	}

	std::vector<std::byte> graphCardinalityKey(uint8_t kind) {
		return {std::byte{13U}, static_cast<std::byte>(kind)};
	}

	void appendBigEndianU32(std::vector<std::byte> &target, uint32_t value) {
		for (uint32_t shift = 24U;; shift -= 8U) {
			target.push_back(static_cast<std::byte>((value >> shift) & UINT32_C(0xFF)));
			if (shift == 0U) {
				return;
			}
		}
	}

	void appendBigEndianU64(std::vector<std::byte> &target, uint64_t value) {
		for (uint32_t shift = 56U;; shift -= 8U) {
			target.push_back(static_cast<std::byte>((value >> shift) & UINT64_C(0xFF)));
			if (shift == 0U) {
				return;
			}
		}
	}

	std::vector<std::byte> schemaNameKey(SystemCatalogSchemaTokenKind kind, std::string_view name) {
		std::vector<std::byte> key = {std::byte{3U}, static_cast<std::byte>(kind)};
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	std::vector<std::byte> schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token, std::string_view name) {
		std::vector<std::byte> key = {std::byte{4U}, static_cast<std::byte>(kind)};
		appendBigEndianU64(key, token);
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	void appendLengthPrefixedString(std::vector<std::byte> &target, std::string_view value) {
		target.push_back(static_cast<std::byte>((value.size() >> 8U) & UINT16_C(0xFF)));
		target.push_back(static_cast<std::byte>(value.size() & UINT16_C(0xFF)));
		for (const char character: value) {
			target.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
	}

	std::vector<std::byte> indexDefinitionKey(const IndexDefinition &definition) {
		std::vector<std::byte> key = {std::byte{6U}};
		appendBigEndianU64(key, definition.index_id);
		const std::vector<std::byte> encoded = IndexDefinitionCodec::encode(definition);
		key.insert(key.end(), encoded.begin(), encoded.end());
		return key;
	}

	std::vector<std::byte> constraintDefinitionKey(const ConstraintDefinition &definition) {
		std::vector<std::byte> key = {std::byte{9U}};
		appendBigEndianU64(key, definition.constraint_id);
		const std::vector<std::byte> encoded = ConstraintDefinitionCodec::encode(definition);
		key.insert(key.end(), encoded.begin(), encoded.end());
		return key;
	}

	std::vector<std::byte> definitionKey(uint8_t kind, uint64_t keyId, std::span<const std::byte> encoded) {
		std::vector<std::byte> key = {static_cast<std::byte>(kind)};
		appendBigEndianU64(key, keyId);
		key.insert(key.end(), encoded.begin(), encoded.end());
		return key;
	}

	std::vector<std::byte> namedDefinitionKey(uint8_t kind, std::string_view name) {
		std::vector<std::byte> key = {static_cast<std::byte>(kind)};
		for (const char character: name) {
			key.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
		}
		return key;
	}

	std::vector<std::byte> indexSemanticKey(const IndexDefinition &definition) {
		std::vector<std::byte> key = {std::byte{8U}, static_cast<std::byte>(definition.entity_kind),
									  static_cast<std::byte>(definition.kind)};
		appendBigEndianU64(key, definition.label_token_id);
		appendBigEndianU32(key, definition.vector_dimensions);
		if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
			appendLengthPrefixedString(key, definition.vector_metric);
		} else {
			key.insert(key.end(), {std::byte{0U}, std::byte{0U}});
		}
		key.push_back(static_cast<std::byte>((definition.property_names.size() >> 8U) & UINT16_C(0xFF)));
		key.push_back(static_cast<std::byte>(definition.property_names.size() & UINT16_C(0xFF)));
		for (const std::string &property: definition.property_names) {
			appendLengthPrefixedString(key, property);
		}
		return key;
	}

	std::vector<std::byte> constraintSemanticKey(const ConstraintDefinition &definition) {
		std::vector<std::byte> key = {std::byte{11U}, static_cast<std::byte>(definition.entity_kind),
									  static_cast<std::byte>(definition.kind)};
		appendBigEndianU64(key, definition.schema_token_id);
		key.push_back(static_cast<std::byte>(definition.property_type));
		key.push_back(static_cast<std::byte>((definition.property_names.size() >> 8U) & UINT16_C(0xFF)));
		key.push_back(static_cast<std::byte>(definition.property_names.size() & UINT16_C(0xFF)));
		for (const std::string &property: definition.property_names) {
			appendLengthPrefixedString(key, property);
		}
		return key;
	}

	constexpr uint64_t SYSTEM_CATALOG_BOOTSTRAP_VALUE = UINT64_C(0x5A5958434154414C);
	constexpr uint64_t SYSTEM_CATALOG_CONFIGURATION_VALUE = UINT64_C(0x434F4E4649475552);

} // namespace

#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"

TEST(SystemCatalogIdRangeTest, AcceptsOnlyPersistedDefinitionIdRanges) {
	EXPECT_FALSE(isSystemCatalogIndexDefinitionId(RSS_SYSTEM_CATALOG_INDEX_ID_FIRST - 1U));
	EXPECT_TRUE(isSystemCatalogIndexDefinitionId(RSS_SYSTEM_CATALOG_INDEX_ID_FIRST));
	EXPECT_TRUE(isSystemCatalogIndexDefinitionId(RSS_SYSTEM_CATALOG_INDEX_ID_LAST));
	EXPECT_FALSE(isSystemCatalogIndexDefinitionId(RSS_SYSTEM_CATALOG_INDEX_ID_LAST + 1U));

	EXPECT_FALSE(isSystemCatalogConstraintDefinitionId(RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST - 1U));
	EXPECT_TRUE(isSystemCatalogConstraintDefinitionId(RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST));
	EXPECT_TRUE(isSystemCatalogConstraintDefinitionId(RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST));
	EXPECT_FALSE(isSystemCatalogConstraintDefinitionId(RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST + 1U));
}

TEST_F(SystemCatalogTest, ReturnsAbsentMappingsBeforeTheCatalogRootExists) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	EXPECT_TRUE(catalog.usesMetadataStore(*metadata_));
	EXPECT_EQ(&catalog.pageStore(), &indexes);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();

	EXPECT_EQ(catalog.findPropertyToken(snapshot, "name"), std::nullopt);
	EXPECT_EQ(catalog.findPropertyName(snapshot, 1U), std::nullopt);
	EXPECT_EQ(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), std::nullopt);
	EXPECT_EQ(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 1U),
			  std::nullopt);
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST), std::nullopt);
	EXPECT_EQ(catalog.findConstraintDefinitionById(snapshot, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST), std::nullopt);
	EXPECT_TRUE(catalog.readIndexDefinitions(snapshot, 1U).empty());
	EXPECT_TRUE(catalog.readConstraintDefinitions(snapshot, 1U).empty());
	EXPECT_EQ(catalog.findConfiguration(snapshot, "query.timeout_ms"), std::nullopt);
	EXPECT_TRUE(catalog.readConfigurations(snapshot, 1U).empty());
	snapshot.release();
}

TEST_F(SystemCatalogTest, PublishesTypedConfigurationRecordsAtomicallyAndPersistsTheirLatestValue) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();

	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	createSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{30}));
	createSession.setConfiguration("query.slow_log.enabled", PropertyValue(true));
	// Replacing a staged value must leave one canonical record, not two values
	// for the same name in the initial catalog root.
	createSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{45}));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	EXPECT_EQ(catalog.findConfiguration(before, "query.timeout_ms"), std::nullopt);
	before.release();
	MetadataSnapshotPin initial = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(initial, "query.timeout_ms"), PropertyValue(int64_t{45}));
	EXPECT_EQ(catalog.readConfigurations(initial, 4U),
			  (std::vector<NativeConfigurationRecord>{{"query.slow_log.enabled", PropertyValue(true)},
																 {"query.timeout_ms", PropertyValue(int64_t{45})}}));

	NativeGraphWriteTransaction update = planner.beginTransaction();
	SystemCatalogWriteSession updateSession = catalog.beginWrite(update);
	updateSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{60}));
	updateSession.finish();
	ASSERT_TRUE(update.commit().committed());
	EXPECT_EQ(catalog.findConfiguration(initial, "query.timeout_ms"), PropertyValue(int64_t{45}));
	initial.release();

	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	SystemCatalog reopenedCatalog(reopenedIndexes, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(reopenedCatalog.findConfiguration(reopened, "query.timeout_ms"), PropertyValue(int64_t{60}));
	EXPECT_EQ(reopenedCatalog.findConfiguration(reopened, "query.slow_log.enabled"), PropertyValue(true));
	EXPECT_THROW(static_cast<void>(reopenedCatalog.readConfigurations(reopened, 0U)), FormatException);
	reopened.release();
}

TEST_F(SystemCatalogTest, RejectsConfigurationWritesFromMovedOrFinishedSessions) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession source = catalog.beginWrite(transaction);
	source.setConfiguration("session.live", PropertyValue(true));
	SystemCatalogWriteSession active = std::move(source);

	EXPECT_THROW(source.setConfiguration("session.moved", PropertyValue(true)), std::logic_error);
	active.finish();
	EXPECT_THROW(active.setConfiguration("session.finished", PropertyValue(true)), std::logic_error);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(snapshot, "session.live"), PropertyValue(true));
	EXPECT_EQ(catalog.findConfiguration(snapshot, "session.moved"), std::nullopt);
	EXPECT_EQ(catalog.findConfiguration(snapshot, "session.finished"), std::nullopt);
	snapshot.release();
}

TEST_F(SystemCatalogTest, PublishesGraphCardinalityStatisticsAtomicallyAndPersistsThemAcrossReopen) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(before), std::nullopt);

	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	createSession.setGraphCardinalityStatistics({3U, 5U});
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(before), std::nullopt);
	before.release();

	MetadataSnapshotPin first = metadata_->openSnapshot();
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(first),
			  std::optional<NativeGraphCardinalityStatistics>({3U, 5U}));
	first.release();

	NativeGraphWriteTransaction update = planner.beginTransaction();
	SystemCatalogWriteSession updateSession = catalog.beginWrite(update);
	updateSession.setGraphCardinalityStatistics({2U, 4U});
	updateSession.finish();
	ASSERT_TRUE(update.commit().committed());

	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	SystemCatalog reopenedCatalog(reopenedIndexes, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(reopenedCatalog.readGraphCardinalityStatistics(reopened),
			  std::optional<NativeGraphCardinalityStatistics>({2U, 4U}));
	reopened.release();
}

TEST_F(SystemCatalogTest, PersistsZeroGraphCardinalityStatisticsAsACompleteRecord) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	EXPECT_FALSE(session.changed());
	EXPECT_FALSE(session.finished());
	session.setGraphCardinalityStatistics({0U, 0U});
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(snapshot),
			  std::optional<NativeGraphCardinalityStatistics>({0U, 0U}));
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenGraphCardinalityRecordIsPartial) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	const std::array<BTreePageEntry, 2U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{graphCardinalityKey(1U), 3U},
	};
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	seed.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.readGraphCardinalityStatistics(snapshot)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsGraphCardinalityOverflowAndZeroEncodedCounts) {
	EXPECT_THROW(static_cast<void>(SystemCatalogGraphCardinalityCodec::encodeCount(
					 std::numeric_limits<uint64_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(SystemCatalogGraphCardinalityCodec::decodeCount(0U)), FormatException);
}

TEST_F(SystemCatalogTest, PersistsEverySupportedScalarConfigurationValue) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	session.setConfiguration("config.boolean", PropertyValue(true));
	session.setConfiguration("config.integer", PropertyValue(int64_t{-42}));
	session.setConfiguration("config.double", PropertyValue(1.25));
	session.setConfiguration("config.string", PropertyValue("native"));
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(snapshot, "config.boolean"), PropertyValue(true));
	EXPECT_EQ(catalog.findConfiguration(snapshot, "config.integer"), PropertyValue(int64_t{-42}));
	EXPECT_EQ(catalog.findConfiguration(snapshot, "config.double"), PropertyValue(1.25));
	EXPECT_EQ(catalog.findConfiguration(snapshot, "config.string"), PropertyValue("native"));
	snapshot.release();
}

TEST_F(SystemCatalogTest, ReturnsAbsentConfigurationFromAnExistingTokenOnlyCatalog) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	static_cast<void>(session.ensurePropertyToken("token_only"));
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(snapshot, "missing"), std::nullopt);
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, std::numeric_limits<size_t>::max())),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsMalformedOrNonScalarConfigurationRecordsBeforePublication) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	EXPECT_THROW(session.setConfiguration("", PropertyValue(true)), FormatException);
	EXPECT_THROW(session.setConfiguration("query.invalid", PropertyValue(std::vector<PropertyValue>{PropertyValue(true)})),
			 FormatException);
	session.finish();
	transaction.rollback();

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(catalog.readConfigurations(snapshot, 1U).empty());
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenPersistedConfigurationEntriesAreMalformed) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	session.setConfiguration("valid", PropertyValue(true));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	std::vector<std::byte> trailing = configurationKey("trail", PropertyValue(true));
	trailing.push_back(std::byte{0U});
	const std::array truncatedLength{std::byte{12U}, std::byte{0U}};
	const std::vector<std::byte> invalidName =
			{std::byte{12U}, std::byte{0U}, std::byte{0U}, std::byte{0U}, std::byte{0U}};
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, truncatedLength,
							  SYSTEM_CATALOG_CONFIGURATION_VALUE);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, configurationPrefix("short"),
							  SYSTEM_CATALOG_CONFIGURATION_VALUE);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, trailing, SYSTEM_CATALOG_CONFIGURATION_VALUE);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, configurationKey("wrong", PropertyValue(true)), 0U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, invalidName,
							  SYSTEM_CATALOG_CONFIGURATION_VALUE);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConfiguration(snapshot, "short")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConfiguration(snapshot, "trail")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConfiguration(snapshot, "wrong")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, 8U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsInvalidConfigurationValueMarkersDuringEnumeration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	const std::array<BTreePageEntry, 2U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{configurationKey("invalid_marker", PropertyValue(true)), 1U},
	};
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	seed.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, 1U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsPersistedConfigurationNamesThatExceedTheCanonicalBound) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	session.setConfiguration("valid", PropertyValue(true));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	// The writer rejects this name, but recovery must reject a structurally valid
	// B+Tree entry whose persisted length exceeds the catalog grammar.
	const std::string oversized(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, 'x');
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
								   configurationKey(oversized, PropertyValue(true)),
								   SYSTEM_CATALOG_CONFIGURATION_VALUE);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, 8U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsPersistedConfigurationKeysWithTruncatedNamesOrNonScalarValues) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	session.setConfiguration("valid", PropertyValue(true));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	const std::array truncatedName{std::byte{12U}, std::byte{0U}, std::byte{0U},
								   std::byte{0U},  std::byte{2U}, std::byte{'x'}};
	NativeGraphWriteTransaction truncated = planner.beginTransaction();
	truncated.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, truncatedName,
									 SYSTEM_CATALOG_CONFIGURATION_VALUE);
	ASSERT_TRUE(truncated.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, 8U)), FormatException);
	snapshot.release();

	// A later generation isolates the scalar-type validation from the preceding
	// malformed key while retaining the same public catalog read boundary.
	NativeGraphWriteTransaction nonScalar = planner.beginTransaction();
	nonScalar.stageScalarIndexUpsert(
			indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
			configurationKey("list", PropertyValue(std::vector<PropertyValue>{PropertyValue(true)})),
			SYSTEM_CATALOG_CONFIGURATION_VALUE);
	ASSERT_TRUE(nonScalar.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConfiguration(current, "list")), FormatException);
	current.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenPersistedDefinitionKeysAreTruncated) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	session.setConfiguration("valid", PropertyValue(true));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	// A B+Tree page can be structurally valid while an entry violates the
	// System Catalog's subtype key grammar. Readers must reject it before
	// exposing a partial definition inventory.
	const std::array truncatedIndexKey{std::byte{6U}};
	const std::array truncatedConstraintKey{std::byte{9U}};
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, truncatedIndexKey, 1U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, truncatedConstraintKey, 1U);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 8U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, 8U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsAmbiguousOrBoundedPersistedConfigurationRecords) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	session.setConfiguration("alpha", PropertyValue(true));
	session.setConfiguration("bravo", PropertyValue(int64_t{7}));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin beforeCorruption = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(beforeCorruption, "absent"), std::nullopt);
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(beforeCorruption, 1U)), FormatException);
	beforeCorruption.release();

	// A second value with the same name is a persisted corruption, not an
	// implicit last-write-wins update. Inject it through the production COW
	// transaction path so the read-side validation sees a durable tree state.
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
							  configurationKey("alpha", PropertyValue(false)),
							  SYSTEM_CATALOG_CONFIGURATION_VALUE);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConfiguration(snapshot, "alpha")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConfigurations(snapshot, 8U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, PublishesTokenMappingsWithNativeGraphChangesInOneGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	const uint32_t property = session.ensurePropertyToken("name");
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	const uint64_t node = transaction.createNode(std::array<uint64_t, 1U>{label});
	transaction.replaceNodeProperty({node, Node::typeId, {{property, PropertyValue("Ada")}}});
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	EXPECT_EQ(catalog.findPropertyToken(before, "name"), std::nullopt);
	EXPECT_EQ(catalog.findSchemaToken(before, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), std::nullopt);
	before.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(current, "name"), property);
	EXPECT_EQ(catalog.findPropertyName(current, property), std::optional<std::string>("name"));
	EXPECT_EQ(catalog.findPropertyName(current, property + 1U), std::nullopt);
	EXPECT_EQ(catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), label);
	EXPECT_EQ(catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label),
			  std::optional<SystemCatalogSchemaToken>({label, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"}));
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, node}).entries,
			  (std::vector<PropertyCollectionRootEntry>{{property, PropertyValue("Ada")}}));
	current.release();
}

TEST_F(SystemCatalogTest, KeepsLabelAndRelationshipTypeNamesInSeparateTokenDomains) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "RELATES_TO");
	const uint64_t relationship =
			session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "RELATES_TO");
	EXPECT_EQ(label, session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "RELATES_TO"));
	EXPECT_EQ(relationship,
			  session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "RELATES_TO"));
	EXPECT_NE(label, relationship);
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "RELATES_TO"), label);
	EXPECT_EQ(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "RELATES_TO"),
			  relationship);
	EXPECT_EQ(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, relationship), std::nullopt);
	snapshot.release();
}

TEST_F(SystemCatalogTest, PersistsForwardAndReverseMappingsAcrossReopenAndAdvancesSequences) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction firstTransaction = planner.beginTransaction();
	SystemCatalogWriteSession firstSession = catalog.beginWrite(firstTransaction);
	const uint32_t firstProperty = firstSession.ensurePropertyToken("first");
	const uint64_t firstSchema = firstSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "First");
	EXPECT_EQ(firstSession.ensurePropertyToken("first"), firstProperty);
	EXPECT_EQ(firstSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "First"), firstSchema);
	firstSession.finish();
	ASSERT_TRUE(firstTransaction.commit().committed());

	NativeGraphWriteTransaction secondTransaction = planner.beginTransaction();
	SystemCatalogWriteSession secondSession = catalog.beginWrite(secondTransaction);
	EXPECT_EQ(secondSession.ensurePropertyToken("first"), firstProperty);
	EXPECT_EQ(secondSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "First"), firstSchema);
	const uint32_t secondProperty = secondSession.ensurePropertyToken("second");
	const uint64_t secondSchema =
			secondSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "SECOND");
	secondSession.finish();
	ASSERT_TRUE(secondTransaction.commit().committed());
	EXPECT_EQ(secondProperty, firstProperty + 1U);
	EXPECT_EQ(secondSchema, firstSchema + 1U);

	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	SystemCatalog reopenedCatalog(reopenedIndexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(reopenedCatalog.findPropertyName(snapshot, firstProperty), std::optional<std::string>("first"));
	EXPECT_EQ(reopenedCatalog.findPropertyToken(snapshot, "second"), secondProperty);
	EXPECT_EQ(
			reopenedCatalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, firstSchema),
			std::optional<SystemCatalogSchemaToken>({firstSchema, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "First"}));
	EXPECT_EQ(reopenedCatalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE,
											  secondSchema),
			  std::optional<SystemCatalogSchemaToken>(
					  {secondSchema, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "SECOND"}));
	snapshot.release();
}

TEST_F(SystemCatalogTest, RequiresFinishBeforeInitialRootPublication) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	static_cast<void>(session.ensurePropertyToken("unpublished"));
	static_cast<void>(transaction.createNode(std::array<uint64_t, 0U>{}));
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(snapshot, "unpublished"), std::nullopt);
	EXPECT_FALSE(metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsInvalidAndFinishedWriteSessionRequests) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);

	EXPECT_THROW(static_cast<void>(session.ensurePropertyToken("")), FormatException);
	EXPECT_THROW(static_cast<void>(session.ensureSchemaToken(static_cast<SystemCatalogSchemaTokenKind>(0xffU), "Person")),
				 FormatException);
	IndexDefinition suppliedIndexId;
	suppliedIndexId.index_id = 1U;
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(std::move(suppliedIndexId))), FormatException);
	ConstraintDefinition suppliedConstraintId;
	suppliedConstraintId.constraint_id = 1U;
	EXPECT_THROW(static_cast<void>(session.createConstraintDefinition(std::move(suppliedConstraintId))), FormatException);

	static_cast<void>(session.ensurePropertyToken("name"));
	EXPECT_TRUE(session.changed());
	session.finish();
	EXPECT_TRUE(session.finished());
	session.finish();
	EXPECT_THROW(static_cast<void>(session.ensurePropertyToken("after_finish")), std::logic_error);
	EXPECT_THROW(static_cast<void>(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "after_finish")),
			 std::logic_error);
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(IndexDefinition{})), std::logic_error);
	EXPECT_THROW(static_cast<void>(session.createConstraintDefinition(ConstraintDefinition{})), std::logic_error);
	EXPECT_THROW(session.dropIndexDefinition(IndexDefinition{}), std::logic_error);
	EXPECT_THROW(session.dropConstraintDefinition(ConstraintDefinition{}), std::logic_error);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalogWriteSession detached;
	EXPECT_THROW(detached.finish(), std::logic_error);
}

static_assert(std::is_move_constructible_v<SystemCatalogWriteSession>);
static_assert(!std::is_move_assignable_v<SystemCatalogWriteSession>);

TEST_F(SystemCatalogTest, RejectsInvalidOrDuplicateDefinitionsWithoutConsumingDurableIds) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	IndexDefinition invalidIndex;
	invalidIndex.name = "invalid_idx";
	invalidIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	invalidIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(std::move(invalidIndex))), FormatException);

	IndexDefinition firstIndex;
	firstIndex.name = "first_idx";
	firstIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	firstIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	firstIndex.property_names = {"email"};
	firstIndex = session.createIndexDefinition(std::move(firstIndex));
	EXPECT_EQ(firstIndex.index_id, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST);
	IndexDefinition duplicateIndex = firstIndex;
	duplicateIndex.index_id = 0U;
	duplicateIndex.name = "duplicate_idx";
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(std::move(duplicateIndex))), FormatException);
	IndexDefinition secondIndex = firstIndex;
	secondIndex.index_id = 0U;
	secondIndex.name = "second_idx";
	secondIndex.property_names = {"name"};
	secondIndex = session.createIndexDefinition(std::move(secondIndex));
	EXPECT_EQ(secondIndex.index_id, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U);

	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	ConstraintDefinition invalidConstraint;
	invalidConstraint.name = "invalid_constraint";
	invalidConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	invalidConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	invalidConstraint.schema_token_id = label;
	EXPECT_THROW(static_cast<void>(session.createConstraintDefinition(std::move(invalidConstraint))), FormatException);

	ConstraintDefinition firstConstraint;
	firstConstraint.name = "first_unique";
	firstConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	firstConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	firstConstraint.schema_token_id = label;
	firstConstraint.property_names = {"email"};
	firstConstraint = session.createConstraintDefinition(std::move(firstConstraint));
	EXPECT_EQ(firstConstraint.constraint_id, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST);
	ConstraintDefinition duplicateConstraint = firstConstraint;
	duplicateConstraint.constraint_id = 0U;
	duplicateConstraint.name = "duplicate_unique";
	EXPECT_THROW(static_cast<void>(session.createConstraintDefinition(std::move(duplicateConstraint))), FormatException);
	ConstraintDefinition secondConstraint = firstConstraint;
	secondConstraint.constraint_id = 0U;
	secondConstraint.name = "second_unique";
	secondConstraint.property_names = {"name"};
	secondConstraint = session.createConstraintDefinition(std::move(secondConstraint));
	EXPECT_EQ(secondConstraint.constraint_id, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U);

	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.readIndexDefinitions(snapshot, 4U),
			  (std::vector<IndexDefinition>{firstIndex, secondIndex}));
	EXPECT_EQ(catalog.readConstraintDefinitions(snapshot, 4U),
			  (std::vector<ConstraintDefinition>{firstConstraint, secondConstraint}));
	snapshot.release();
}

TEST_F(SystemCatalogTest, KeepsAnActiveCatalogSessionUsableAfterMoveConstruction) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession original = catalog.beginWrite(transaction);
	SystemCatalogWriteSession moved(std::move(original));
	EXPECT_THROW(static_cast<void>(original.ensurePropertyToken("moved_from_constructor")), std::logic_error);
	EXPECT_THROW(static_cast<void>(original.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Moved")),
			 std::logic_error);
	EXPECT_THROW(static_cast<void>(original.createIndexDefinition(IndexDefinition{})), std::logic_error);
	EXPECT_THROW(static_cast<void>(original.createConstraintDefinition(ConstraintDefinition{})), std::logic_error);
	EXPECT_THROW(original.setConfiguration("moved_from_configuration", PropertyValue{}), std::logic_error);
	EXPECT_THROW(original.setGraphCardinalityStatistics({}), std::logic_error);
	EXPECT_THROW(original.dropIndexDefinition(IndexDefinition{}), std::logic_error);
	EXPECT_THROW(original.dropConstraintDefinition(ConstraintDefinition{}), std::logic_error);
	const uint32_t token = moved.ensurePropertyToken("moved");
	moved.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(snapshot, "moved"), token);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenPersistedAllocationSequencesAreOutOfRange) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 4U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{{std::byte{5U}, std::byte{1U}}, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U},
			BTreePageEntry{{std::byte{5U}, std::byte{3U}}, RSS_SYSTEM_CATALOG_INDEX_ID_LAST + 1U},
			BTreePageEntry{{std::byte{5U}, std::byte{4U}}, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST + 1U},
	};
	seed.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession propertySession = catalog.beginWrite(transaction);
	EXPECT_THROW(static_cast<void>(propertySession.ensurePropertyToken("name")), FormatException);
	SystemCatalogWriteSession indexSession = catalog.beginWrite(transaction);
	EXPECT_THROW(static_cast<void>(indexSession.createIndexDefinition(IndexDefinition{})), FormatException);
	SystemCatalogWriteSession constraintSession = catalog.beginWrite(transaction);
	EXPECT_THROW(static_cast<void>(constraintSession.createConstraintDefinition(ConstraintDefinition{})), FormatException);
}

TEST_F(SystemCatalogTest, RollsOverSchemaTokenSequenceAndRejectsSubsequentExhaustion) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 2U> entries = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{{std::byte{5U}, std::byte{2U}}, std::numeric_limits<uint64_t>::max()},
	};
	seed.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	EXPECT_EQ(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "MaxToken"),
			  std::numeric_limits<uint64_t>::max());
	// The boundary allocation is valid and persists zero as the explicit
	// exhausted marker; a second allocation must fail closed.
	EXPECT_THROW(static_cast<void>(session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "AfterMax")),
				 FormatException);
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());
}

TEST_F(SystemCatalogTest, RollsOverIndexAndConstraintSequencesAtTheirUpperBounds) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<BTreePageEntry, 3U> entries = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{{std::byte{5U}, std::byte{3U}}, RSS_SYSTEM_CATALOG_INDEX_ID_LAST},
			BTreePageEntry{{std::byte{5U}, std::byte{4U}}, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST},
	};
	seed.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(seed.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	static_cast<void>(session.ensurePropertyToken("email"));
	static_cast<void>(session.ensurePropertyToken("name"));
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");

	IndexDefinition firstIndex;
	firstIndex.name = "first_idx";
	firstIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	firstIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	firstIndex.property_names = {"email"};
	EXPECT_EQ(session.createIndexDefinition(std::move(firstIndex)).index_id, RSS_SYSTEM_CATALOG_INDEX_ID_LAST);
	IndexDefinition secondIndex;
	secondIndex.name = "second_idx";
	secondIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	secondIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	secondIndex.property_names = {"name"};
	EXPECT_THROW(static_cast<void>(session.createIndexDefinition(std::move(secondIndex))), FormatException);

	ConstraintDefinition firstConstraint;
	firstConstraint.name = "first_unique";
	firstConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	firstConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	firstConstraint.schema_token_id = label;
	firstConstraint.property_names = {"email"};
	EXPECT_EQ(session.createConstraintDefinition(std::move(firstConstraint)).constraint_id,
			  RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST);
	ConstraintDefinition secondConstraint;
	secondConstraint.name = "second_unique";
	secondConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	secondConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	secondConstraint.schema_token_id = label;
	secondConstraint.property_names = {"name"};
	EXPECT_THROW(static_cast<void>(session.createConstraintDefinition(std::move(secondConstraint))), FormatException);
}

TEST_F(SystemCatalogTest, RejectsStagedAndDurableDefinitionConflicts) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.property_names = {"email"};

	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	constraint.schema_token_id =
			createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	index = createSession.createIndexDefinition(std::move(index));
	constraint = createSession.createConstraintDefinition(std::move(constraint));
	IndexDefinition duplicateStagedIndex = index;
	duplicateStagedIndex.index_id = 0U;
	duplicateStagedIndex.name = "same_index_schema_different_name";
	ConstraintDefinition duplicateStagedConstraint = constraint;
	duplicateStagedConstraint.constraint_id = 0U;
	duplicateStagedConstraint.name = "same_constraint_schema_different_name";
	EXPECT_THROW(static_cast<void>(createSession.createIndexDefinition(std::move(duplicateStagedIndex))), FormatException);
	EXPECT_THROW(static_cast<void>(createSession.createConstraintDefinition(std::move(duplicateStagedConstraint))),
				 FormatException);
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction duplicate = planner.beginTransaction();
	SystemCatalogWriteSession duplicateSession = catalog.beginWrite(duplicate);
	IndexDefinition duplicateDurableIndex = index;
	duplicateDurableIndex.index_id = 0U;
	duplicateDurableIndex.name = "same_durable_index_schema_different_name";
	ConstraintDefinition duplicateDurableConstraint = constraint;
	duplicateDurableConstraint.constraint_id = 0U;
	duplicateDurableConstraint.name = "same_durable_constraint_schema_different_name";
	EXPECT_THROW(static_cast<void>(duplicateSession.createIndexDefinition(std::move(duplicateDurableIndex))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(duplicateSession.createConstraintDefinition(std::move(duplicateDurableConstraint))),
				 FormatException);
	IndexDefinition mismatchedIndex = index;
	mismatchedIndex.name = "wrong_name";
	ConstraintDefinition mismatchedConstraint = constraint;
	mismatchedConstraint.name = "wrong_name";
	EXPECT_THROW(duplicateSession.dropIndexDefinition(mismatchedIndex), FormatException);
	EXPECT_THROW(duplicateSession.dropConstraintDefinition(mismatchedConstraint), FormatException);
	IndexDefinition missingIndex;
	missingIndex.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 100U;
	ConstraintDefinition missingConstraint;
	missingConstraint.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 100U;
	EXPECT_THROW(duplicateSession.dropIndexDefinition(missingIndex), FormatException);
	EXPECT_THROW(duplicateSession.dropConstraintDefinition(missingConstraint), FormatException);
}

TEST_F(SystemCatalogTest, RejectsStaleFirstRootPublicationWithoutLeakingMappings) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction winner = planner.beginTransaction();
	NativeGraphWriteTransaction stale = planner.beginTransaction();
	SystemCatalogWriteSession winnerSession = catalog.beginWrite(winner);
	SystemCatalogWriteSession staleSession = catalog.beginWrite(stale);
	const uint32_t winnerToken = winnerSession.ensurePropertyToken("winner");
	static_cast<void>(staleSession.ensurePropertyToken("stale"));
	winnerSession.finish();
	staleSession.finish();
	ASSERT_TRUE(winner.commit().committed());
	EXPECT_FALSE(stale.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(snapshot, "winner"), winnerToken);
	EXPECT_EQ(catalog.findPropertyToken(snapshot, "stale"), std::nullopt);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RecoversTheOldOrNewConfigurationValueAcrossInterruptedReplacementPublication) {
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		NativeGraphWriteTransaction create = planner.beginTransaction();
		SystemCatalogWriteSession createSession = catalog.beginWrite(create);
		createSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{30}));
		createSession.finish();
		ASSERT_TRUE(create.commit().committed());

		NativeGraphWriteTransaction interrupted = planner.beginTransaction();
		SystemCatalogWriteSession interruptedSession = catalog.beginWrite(interrupted);
		interruptedSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{60}));
		interruptedSession.finish();
		MetadataPublishOptions beforePublication;
		beforePublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interrupted.commit(beforePublication)), BootstrapPublishInterrupted);
		EXPECT_EQ(interrupted.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(catalog.findConfiguration(current, "query.timeout_ms"), PropertyValue(int64_t{30}));
		current.release();
	}

	reopenNativeStores();
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		MetadataSnapshotPin recoveredOld = metadata_->openSnapshot();
		EXPECT_EQ(catalog.findConfiguration(recoveredOld, "query.timeout_ms"), PropertyValue(int64_t{30}));
		recoveredOld.release();

		NativeGraphWriteTransaction interrupted = planner.beginTransaction();
		SystemCatalogWriteSession interruptedSession = catalog.beginWrite(interrupted);
		interruptedSession.setConfiguration("query.timeout_ms", PropertyValue(int64_t{60}));
		interruptedSession.finish();
		MetadataPublishOptions afterPublication;
		afterPublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(interrupted.commit(afterPublication)), BootstrapPublishInterrupted);
		EXPECT_EQ(interrupted.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}

	reopenNativeStores();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin recoveredNew = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findConfiguration(recoveredNew, "query.timeout_ms"), PropertyValue(int64_t{60}));
	EXPECT_EQ(catalog.readConfigurations(recoveredNew, 2U),
			  (std::vector<NativeConfigurationRecord>{{"query.timeout_ms", PropertyValue(int64_t{60})}}));
	recoveredNew.release();
}

TEST_F(SystemCatalogTest, RecoversTheOldCatalogGenerationWhenFirstPublicationStopsBeforeSuperblock) {
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		static_cast<void>(session.ensurePropertyToken("interrupted"));
		static_cast<void>(transaction.createNode(std::array<uint64_t, 0U>{}));
		session.finish();
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(transaction.commit(options)), BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(catalog.findPropertyToken(current, "interrupted"), std::nullopt);
		EXPECT_FALSE(metadata_->findIndexRootOptional(current, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
		current.release();
	}

	reopenNativeStores();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(recovered, "interrupted"), std::nullopt);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	recovered.release();
}

TEST_F(SystemCatalogTest, RecoversTheNewCatalogGenerationWhenFirstPublicationStopsAfterSuperblock) {
	uint32_t publishedToken = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		SystemCatalog catalog(indexes, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		SystemCatalogWriteSession session = catalog.beginWrite(transaction);
		publishedToken = session.ensurePropertyToken("published");
		static_cast<void>(transaction.createNode(std::array<uint64_t, 0U>{}));
		session.finish();
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(transaction.commit(options)), BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(catalog.findPropertyToken(current, "published"), publishedToken);
		current.release();
	}

	reopenNativeStores();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findPropertyToken(recovered, "published"), publishedToken);
	EXPECT_EQ(catalog.findPropertyName(recovered, publishedToken), std::optional<std::string>("published"));
	recovered.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAReachableCatalogHasNoBootstrapRecord) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 1U> malformed = {{{propertyNameKey("name"), 1U}}};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, "name")), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAPropertyForwardMappingHasNoOrMismatchedReverseMapping) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 4U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{propertyNameKey("name"), 1U},
		BTreePageEntry{propertyNameKey("wrong-owner"), 2U},
		// Token 2 is present, but its reverse record must preserve the queried name.
		BTreePageEntry{propertyTokenKey(2U, "another-name"), 2U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, "name")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, "wrong-owner")), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, ReturnsAbsentStatisticsForABootstrappedCatalogWithoutStatistics) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 1U> entries = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(snapshot), std::nullopt);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAPropertyReverseMappingIsMissingOrAmbiguous) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 3U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{propertyTokenKey(1U, "alpha"), 1U},
			BTreePageEntry{propertyTokenKey(1U, "beta"), 1U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 1U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAPropertyReverseRecordHasNoNameOrWrongTokenValue) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 3U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			// The key is exactly the reverse-token prefix, so it cannot identify a name.
			BTreePageEntry{propertyTokenKey(1U, ""), 1U},
			// Token 2 must map back to itself rather than to another token's value.
			BTreePageEntry{propertyTokenKey(2U, "wrong-owner"), 1U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 2U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAPropertyReverseMappingHasNoOrMismatchedForwardMapping) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 4U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
		BTreePageEntry{propertyNameKey("wrong-owner"), 3U},
			BTreePageEntry{propertyTokenKey(1U, "name"), 1U},
			// The forward key exists, but it must point back to its own reverse token.
			BTreePageEntry{propertyTokenKey(2U, "wrong-owner"), 2U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 1U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 2U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenAPropertyForwardMappingExceedsTheTokenWidth) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 2U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{propertyNameKey("name"), static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, "name")), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsInvalidPublicTokenLookupArguments) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::string oversized(RSS_SYSTEM_CATALOG_NAME_MAX_BYTES + 1U, 'x');

	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, "")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyToken(snapshot, oversized)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, static_cast<SystemCatalogSchemaTokenKind>(0xffU), "x")),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "")),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 0U)),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenSchemaForwardOrReverseMappingsAreIncompleteOrInconsistent) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 6U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{schemaNameKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person"), 1U},
		BTreePageEntry{schemaNameKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "another-name"), 3U},
		BTreePageEntry{schemaNameKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "wrong-owner"), 2U},
		BTreePageEntry{schemaTokenKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, 2U, "another-name"), 2U},
		BTreePageEntry{schemaTokenKey(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 2U, "KNOWS"), 2U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person")),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 2U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "wrong-owner")),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 2U)),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenDefinitionEntriesAreTruncatedOrUseDifferentPayloadIds) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	IndexDefinition mismatchedIndex;
	mismatchedIndex.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 2U;
	mismatchedIndex.name = "mismatched_idx";
	mismatchedIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	mismatchedIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	mismatchedIndex.property_names = {"email"};
	ConstraintDefinition mismatchedConstraint;
	mismatchedConstraint.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 2U;
	mismatchedConstraint.name = "mismatched_unique";
	mismatchedConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	mismatchedConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	mismatchedConstraint.schema_token_id = 1U;
	mismatchedConstraint.property_names = {"email"};
	const std::vector<std::byte> encodedIndex = IndexDefinitionCodec::encode(mismatchedIndex);
	const std::vector<std::byte> encodedConstraint = ConstraintDefinitionCodec::encode(mismatchedConstraint);
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<BTreePageEntry, 5U> malformed = {
			BTreePageEntry{{std::byte{0U}}, SYSTEM_CATALOG_BOOTSTRAP_VALUE},
			BTreePageEntry{definitionKey(6U, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST, std::span<const std::byte>{}),
							RSS_SYSTEM_CATALOG_INDEX_ID_FIRST},
			BTreePageEntry{definitionKey(6U, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U, encodedIndex),
							RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U},
			BTreePageEntry{definitionKey(9U, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST, std::span<const std::byte>{}),
							RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST},
			BTreePageEntry{definitionKey(9U, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U, encodedConstraint),
							RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U},
	};
	transaction.stageScalarIndexCreate(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, malformed);
	ASSERT_TRUE(transaction.commit().committed());

	SystemCatalog catalog(indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(
				catalog.findConstraintDefinitionById(snapshot, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U)),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, PublishesAndFindsVectorIndexDefinitionsByTheirCompleteSemanticKey) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Document");
	IndexDefinition vector;
	vector.name = "document_embedding_idx";
	vector.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	vector.kind = IndexDefinitionKind::IDK_VECTOR;
	vector.label_token_id = label;
	vector.property_names = {"embedding"};
	vector.vector_dimensions = 4U;
	vector.vector_metric = "cosine";
	vector = session.createIndexDefinition(std::move(vector));
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findIndexDefinitionBySchema(snapshot, vector.entity_kind, vector.kind, vector.label_token_id,
													 vector.property_names, vector.vector_dimensions, vector.vector_metric),
			  std::optional<IndexDefinition>(vector));
	EXPECT_EQ(catalog.findIndexDefinitionBySchema(snapshot, vector.entity_kind, vector.kind, vector.label_token_id,
													 vector.property_names, vector.vector_dimensions, "l2"),
			  std::nullopt);
	snapshot.release();
}

TEST_F(SystemCatalogTest, PublishesIndexAndConstraintDefinitionsInTheCanonicalCatalogRoot) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	static_cast<void>(session.ensurePropertyToken("email"));

	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.label_token_id = label;
	index.property_names = {"email"};
	index = session.createIndexDefinition(std::move(index));

	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.schema_token_id = label;
	constraint.property_names = {"email"};
	constraint.property_type = ConstraintDefinitionPropertyType::CDPT_NONE;
	constraint = session.createConstraintDefinition(std::move(constraint));
	EXPECT_NE(index.index_id, 0U);
	EXPECT_NE(constraint.constraint_id, 0U);
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, index.index_id), std::optional<IndexDefinition>(index));
	EXPECT_EQ(catalog.findIndexDefinitionByName(snapshot, index.name), std::optional<IndexDefinition>(index));
	EXPECT_EQ(catalog.findIndexDefinitionBySchema(snapshot, index.entity_kind, index.kind, index.label_token_id,
													 index.property_names),
			  std::optional<IndexDefinition>(index));
	EXPECT_EQ(catalog.findIndexDefinitionBySchema(snapshot, index.entity_kind, index.kind, index.label_token_id,
													 std::array<std::string, 1U>{"different"}),
			  std::nullopt);
	EXPECT_EQ(catalog.readIndexDefinitions(snapshot, 4U), std::vector<IndexDefinition>{index});
	EXPECT_EQ(catalog.findConstraintDefinitionById(snapshot, constraint.constraint_id),
			  std::optional<ConstraintDefinition>(constraint));
	EXPECT_EQ(catalog.findConstraintDefinitionByName(snapshot, constraint.name),
			  std::optional<ConstraintDefinition>(constraint));
	EXPECT_EQ(catalog.findConstraintDefinitionBySchema(snapshot, constraint.entity_kind, constraint.kind,
													 constraint.schema_token_id, constraint.property_names,
													 constraint.property_type),
			  std::optional<ConstraintDefinition>(constraint));
	EXPECT_EQ(catalog.findConstraintDefinitionBySchema(snapshot, constraint.entity_kind, constraint.kind,
													 constraint.schema_token_id, std::array<std::string, 1U>{"different"},
													 constraint.property_type),
			  std::nullopt);
	EXPECT_EQ(catalog.readConstraintDefinitions(snapshot, 4U), std::vector<ConstraintDefinition>{constraint});
	snapshot.release();
}

TEST_F(SystemCatalogTest, RejectsInvalidDefinitionLookupAndEnumerationBounds) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(transaction);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");

	IndexDefinition firstIndex;
	firstIndex.name = "first_idx";
	firstIndex.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	firstIndex.kind = IndexDefinitionKind::IDK_PROPERTY;
	firstIndex.label_token_id = label;
	firstIndex.property_names = {"first"};
	firstIndex = session.createIndexDefinition(std::move(firstIndex));
	IndexDefinition secondIndex = firstIndex;
	secondIndex.index_id = 0U;
	secondIndex.name = "second_idx";
	secondIndex.property_names = {"second"};
	secondIndex = session.createIndexDefinition(std::move(secondIndex));

	ConstraintDefinition firstConstraint;
	firstConstraint.name = "first_unique";
	firstConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	firstConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	firstConstraint.schema_token_id = label;
	firstConstraint.property_names = {"first"};
	firstConstraint = session.createConstraintDefinition(std::move(firstConstraint));
	ConstraintDefinition secondConstraint = firstConstraint;
	secondConstraint.constraint_id = 0U;
	secondConstraint.name = "second_unique";
	secondConstraint.property_names = {"second"};
	secondConstraint = session.createConstraintDefinition(std::move(secondConstraint));
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, 0U)), FormatException);
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, 1U), std::nullopt);
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID_LAST + 1U), std::nullopt);
	EXPECT_EQ(catalog.findIndexDefinitionById(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 100U), std::nullopt);
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionByName(snapshot, "")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, std::numeric_limits<size_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 1U)), FormatException);

	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, 0U)), FormatException);
	EXPECT_EQ(catalog.findConstraintDefinitionById(snapshot, 1U), std::nullopt);
	EXPECT_EQ(catalog.findConstraintDefinitionById(snapshot, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST + 1U), std::nullopt);
	EXPECT_EQ(catalog.findConstraintDefinitionById(snapshot, RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 100U), std::nullopt);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionByName(snapshot, "")), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, 0U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, std::numeric_limits<size_t>::max())),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, 1U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenIndexDefinitionDirectionsAreMissingOrDuplicated) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	index = createSession.createIndexDefinition(std::move(index));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, namedDefinitionKey(7U, index.name));
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, index.index_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenIndexNameDirectionTargetsAnotherDefinition) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);

	IndexDefinition first;
	first.name = "person_email_idx";
	first.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	first.kind = IndexDefinitionKind::IDK_PROPERTY;
	first.property_names = {"email"};
	first = session.createIndexDefinition(std::move(first));
	IndexDefinition second = first;
	second.index_id = 0U;
	second.name = "person_name_idx";
	second.property_names = {"name"};
	second = session.createIndexDefinition(std::move(second));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
								  namedDefinitionKey(7U, first.name), second.index_id);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, first.index_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionByName(snapshot, first.name)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenIndexSemanticDirectionTargetsAnotherDefinition) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);

	IndexDefinition first;
	first.name = "person_email_idx";
	first.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	first.kind = IndexDefinitionKind::IDK_PROPERTY;
	first.property_names = {"email"};
	first = session.createIndexDefinition(std::move(first));
	IndexDefinition second = first;
	second.index_id = 0U;
	second.name = "person_name_idx";
	second.property_names = {"name"};
	second = session.createIndexDefinition(std::move(second));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, indexSemanticKey(first), second.index_id);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionBySchema(
				 snapshot, first.entity_kind, first.kind, first.label_token_id, first.property_names,
				 first.vector_dimensions, first.vector_metric)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenConstraintSemanticDirectionIsMissing) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.schema_token_id = createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	constraint.property_names = {"email"};
	constraint = createSession.createConstraintDefinition(std::move(constraint));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, constraintSemanticKey(constraint));
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, constraint.constraint_id)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenConstraintNameDirectionTargetsAnotherDefinition) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");

	ConstraintDefinition first;
	first.name = "person_email_unique";
	first.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	first.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	first.schema_token_id = label;
	first.property_names = {"email"};
	first = session.createConstraintDefinition(std::move(first));
	ConstraintDefinition second = first;
	second.constraint_id = 0U;
	second.name = "person_name_unique";
	second.property_names = {"name"};
	second = session.createConstraintDefinition(std::move(second));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	// The first name still exists, but its direction now points at a different,
	// otherwise valid definition. Both lookup directions must reject that split.
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
								  namedDefinitionKey(10U, first.name), second.constraint_id);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, first.constraint_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionByName(snapshot, first.name)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenConstraintSemanticDirectionTargetsAnotherDefinition) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession session = catalog.beginWrite(create);
	const uint64_t label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");

	ConstraintDefinition first;
	first.name = "person_email_unique";
	first.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	first.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	first.schema_token_id = label;
	first.property_names = {"email"};
	first = session.createConstraintDefinition(std::move(first));
	ConstraintDefinition second = first;
	second.constraint_id = 0U;
	second.name = "person_name_unique";
	second.property_names = {"name"};
	second = session.createConstraintDefinition(std::move(second));
	session.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, constraintSemanticKey(first),
								  second.constraint_id);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionBySchema(
				 snapshot, first.entity_kind, first.kind, first.schema_token_id, first.property_names, first.property_type)),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenIndexSemanticDirectionIsMissing) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	index = createSession.createIndexDefinition(std::move(index));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, indexSemanticKey(index));
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, index.index_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenDefinitionIdsHaveMultiplePersistedRecords) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	index = createSession.createIndexDefinition(std::move(index));
	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.schema_token_id = createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	constraint.property_names = {"email"};
	constraint = createSession.createConstraintDefinition(std::move(constraint));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	IndexDefinition duplicate = index;
	duplicate.name = "duplicate_name";
	ConstraintDefinition duplicateConstraint = constraint;
	duplicateConstraint.name = "duplicate_constraint";
	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, indexDefinitionKey(duplicate), duplicate.index_id);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, constraintDefinitionKey(duplicateConstraint),
								   duplicateConstraint.constraint_id);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, index.index_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, constraint.constraint_id)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenDefinitionRecordsOrLookupDirectionsDisagree) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	index = createSession.createIndexDefinition(std::move(index));
	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.schema_token_id = createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	constraint.property_names = {"email"};
	constraint = createSession.createConstraintDefinition(std::move(constraint));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, indexDefinitionKey(index), index.index_id + 1U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, constraintDefinitionKey(constraint),
								   constraint.constraint_id + 1U);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, index.index_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionById(snapshot, constraint.constraint_id)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.readConstraintDefinitions(snapshot, 4U)), FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, FailsClosedWhenDefinitionNameOrSemanticDirectionsReferenceAnotherId) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition index;
	index.name = "person_email_idx";
	index.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	index.kind = IndexDefinitionKind::IDK_PROPERTY;
	index.property_names = {"email"};
	index = createSession.createIndexDefinition(std::move(index));
	ConstraintDefinition constraint;
	constraint.name = "person_email_unique";
	constraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	constraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	constraint.schema_token_id = createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	constraint.property_names = {"email"};
	constraint = createSession.createConstraintDefinition(std::move(constraint));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction corrupt = planner.beginTransaction();
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, namedDefinitionKey(7U, index.name),
								   RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, indexSemanticKey(index),
								   RSS_SYSTEM_CATALOG_INDEX_ID_FIRST + 1U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, namedDefinitionKey(10U, constraint.name),
								   RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U);
	corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, constraintSemanticKey(constraint),
								   RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U);
	ASSERT_TRUE(corrupt.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionByName(snapshot, index.name)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionBySchema(
				snapshot, index.entity_kind, index.kind, index.label_token_id, index.property_names)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionByName(snapshot, constraint.name)), FormatException);
	EXPECT_THROW(static_cast<void>(catalog.findConstraintDefinitionBySchema(
				snapshot, constraint.entity_kind, constraint.kind, constraint.schema_token_id, constraint.property_names,
				constraint.property_type)),
			 FormatException);
	snapshot.release();
}

TEST_F(SystemCatalogTest, DropsOnlyTheRequestedCanonicalDefinitionDirections) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	SystemCatalogWriteSession createSession = catalog.beginWrite(create);
	IndexDefinition first;
	first.name = "first_idx";
	first.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	first.kind = IndexDefinitionKind::IDK_PROPERTY;
	first.property_names = {"first"};
	first = createSession.createIndexDefinition(std::move(first));
	IndexDefinition second;
	second.name = "second_idx";
	second.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	second.kind = IndexDefinitionKind::IDK_PROPERTY;
	second.property_names = {"second"};
	second = createSession.createIndexDefinition(std::move(second));
	const uint64_t label = createSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	ConstraintDefinition firstConstraint;
	firstConstraint.name = "first_unique";
	firstConstraint.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	firstConstraint.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	firstConstraint.schema_token_id = label;
	firstConstraint.property_names = {"first"};
	firstConstraint = createSession.createConstraintDefinition(std::move(firstConstraint));
	ConstraintDefinition secondConstraint = firstConstraint;
	secondConstraint.constraint_id = 0U;
	secondConstraint.name = "second_unique";
	secondConstraint.property_names = {"second"};
	secondConstraint = createSession.createConstraintDefinition(std::move(secondConstraint));
	createSession.finish();
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction drop = planner.beginTransaction();
	SystemCatalogWriteSession dropSession = catalog.beginWrite(drop);
	dropSession.dropIndexDefinition(first);
	dropSession.dropConstraintDefinition(firstConstraint);
	dropSession.finish();
	ASSERT_TRUE(drop.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(catalog.findIndexDefinitionByName(snapshot, first.name), std::nullopt);
	EXPECT_EQ(catalog.findIndexDefinitionByName(snapshot, second.name), std::optional<IndexDefinition>(second));
	EXPECT_EQ(catalog.readIndexDefinitions(snapshot, 4U), std::vector<IndexDefinition>{second});
	EXPECT_EQ(catalog.findConstraintDefinitionByName(snapshot, firstConstraint.name), std::nullopt);
	EXPECT_EQ(catalog.findConstraintDefinitionByName(snapshot, secondConstraint.name),
			  std::optional<ConstraintDefinition>(secondConstraint));
	EXPECT_EQ(catalog.readConstraintDefinitions(snapshot, 4U), std::vector<ConstraintDefinition>{secondConstraint});
	snapshot.release();
}

TEST_F(SystemCatalogTest, SchemaNameAdmissionPrecedesCopyAndCoversForwardKeyUntilLookupCompletes) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
	auto transaction = planner.beginTransaction();
	auto session = catalog.beginWrite(transaction);
	const std::string name(128U, 't');
	const auto token = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
	const auto propertyToken = session.ensurePropertyToken(name);
	session.finish();
	ASSERT_TRUE(transaction.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	size_t bytes = 0U;
	EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE,
		token, [&](size_t next) { bytes = next; throw std::runtime_error("name allocation rejected"); })), std::runtime_error);
	EXPECT_GE(bytes, name.size());
	size_t peak = 0U;
	std::vector<size_t> admissions;
	const auto found = catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token,
		[&](size_t next) { bytes = next; peak = std::max(peak, next); admissions.push_back(next); });
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->name, name);
	ASSERT_GE(admissions.size(), 3U);
	EXPECT_GE(peak, SystemCatalogTokenKeyCodec::schemaTokenKey(
		SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token, name).size() + name.size());
	EXPECT_EQ(bytes, found->name.capacity());
	EXPECT_GE(peak, found->name.capacity() + name.size() + 2U);
	EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, propertyToken,
		[](size_t) { throw std::runtime_error("Property name rejected"); })), std::runtime_error);
	for (int retry = 0; retry < 2; ++retry) {
		peak = 0U;
		const auto propertyName = catalog.findPropertyName(snapshot, propertyToken,
			[&](size_t next) { bytes = next; peak = std::max(peak, next); });
		ASSERT_TRUE(propertyName.has_value());
		EXPECT_EQ(*propertyName, name);
		EXPECT_EQ(bytes, propertyName->capacity());
		EXPECT_GE(peak, propertyName->capacity() + name.size() + 1U);
	}
	size_t missingCalls = 0U;
	EXPECT_FALSE(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token + 1U,
		[&](size_t) { ++missingCalls; }).has_value());
	EXPECT_EQ(missingCalls, 0U);
}

TEST_F(SystemCatalogTest, BatchedSchemaNameLookupRetainsReverseIntegrityAndPinnedSnapshots) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, plannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 uint64_t token;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   token = session.ensureSchemaToken(kind, "Person"); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto old = metadata_->openSnapshot();
 const auto reverse = SystemCatalogTokenKeyCodec::schemaTokenKey(kind, token, "Person");
 const auto ambiguous = SystemCatalogTokenKeyCodec::schemaTokenKey(kind, token, "Other");
 EXPECT_EQ(catalog.findSchemaToken(old, kind, "Person"), token);
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, ambiguous, token);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto corrupt = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(corrupt, kind, "Person")), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, ambiguous);
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, reverse, token + 1U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto corrupt = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(corrupt, kind, "Person")), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, reverse);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto missing = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(missing, kind, "Person")), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, ambiguous, token);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto mismatched = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(mismatched, kind, "Person")), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                            SystemCatalogTokenKeyCodec::schemaNameKey(kind, "Person"), 0U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto zero = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(zero, kind, "Person")), FormatException); }
 EXPECT_EQ(catalog.findSchemaToken(old, kind, "Person"), token);
 old.release();
}
