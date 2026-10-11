/**
 * @file SystemCatalog.hpp
 * @brief Canonical native System Catalog for schema and Property tokens.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

namespace graph::storage {

	class BTreePageStore;
	class MetadataSnapshotPin;
	class NativeGraphWriteTransaction;
	class RelocatableMetadataStore;

	// This is a persisted API and codec limit, rather than a page-capacity
	// calculation. The B+Tree codec validates the complete namespaced key.
	constexpr uint32_t RSS_SYSTEM_CATALOG_NAME_MAX_BYTES = 1024U;
	constexpr uint64_t RSS_SYSTEM_CATALOG_INDEX_ID = 0x8000000000000020ULL;
	// Logical definition IDs remain disjoint from metadata-owned B+Tree IDs.
	// These ranges are persisted format contracts, not page-capacity guesses.
	constexpr uint64_t RSS_SYSTEM_CATALOG_INDEX_ID_FIRST = UINT64_C(1) << 62U;
	constexpr uint64_t RSS_SYSTEM_CATALOG_INDEX_ID_LAST = (UINT64_C(1) << 63U) - 1U;
	constexpr uint64_t RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST = UINT64_C(0xC000000000000000);
	constexpr uint64_t RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST = UINT64_C(0xDFFFFFFFFFFFFFFF);

	[[nodiscard]] constexpr bool isSystemCatalogIndexDefinitionId(uint64_t id) {
		return id >= RSS_SYSTEM_CATALOG_INDEX_ID_FIRST && id <= RSS_SYSTEM_CATALOG_INDEX_ID_LAST;
	}

	[[nodiscard]] constexpr bool isSystemCatalogConstraintDefinitionId(uint64_t id) {
		return id >= RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST && id <= RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST;
	}

	// Prefixes keep all persisted enum names Windows-safe.
	enum class SystemCatalogSchemaTokenKind : uint8_t {
		SCSTK_LABEL = 1,
		SCSTK_RELATIONSHIP_TYPE = 2,
	};

	struct SystemCatalogSchemaToken {
		uint64_t token_id = 0U;
		SystemCatalogSchemaTokenKind kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
		std::string name;

		bool operator==(const SystemCatalogSchemaToken &) const = default;
	};

	// Configuration is durable database metadata. Runtime counters and process
	// controls intentionally remain outside this record so reopening a file has
	// no hidden process-wide side effects.
	struct NativeConfigurationRecord {
		std::string name;
		PropertyValue value;

		bool operator==(const NativeConfigurationRecord &) const = default;
	};

	// These are graph-visible cardinalities, not allocation high-water marks:
	// rolled-back leases and deleted logical IDs must not inflate an estimate.
	// The two values are persisted under one System Catalog root and published
	// with the graph mutation that changed them.
	struct NativeGraphCardinalityStatistics {
		uint64_t node_count = 0U;
		uint64_t edge_count = 0U;

		bool operator==(const NativeGraphCardinalityStatistics &) const = default;
	};

	// Own the stable token-key layout in one public codec. Streamed ingestion
	// uses these keys without inheriting the ordinary session's name maps.
	class SystemCatalogTokenKeyCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> bootstrapKey();
		[[nodiscard]] static std::vector<std::byte> propertyNameKey(std::string_view name);
		[[nodiscard]] static std::vector<std::byte> propertyTokenKey(uint32_t token, std::string_view name);
		[[nodiscard]] static std::vector<std::byte> schemaNameKey(SystemCatalogSchemaTokenKind kind,
																  std::string_view name);
		[[nodiscard]] static std::vector<std::byte> schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token,
																   std::string_view name);
		[[nodiscard]] static std::vector<std::byte> propertyTokenSequenceKey();
		[[nodiscard]] static std::vector<std::byte> schemaTokenSequenceKey();
		static void validateName(std::string_view name);
		static void validateSchemaKind(SystemCatalogSchemaTokenKind kind);
	};

	// Keep the cardinality record encoding independent from the ordinary token
	// overlay so streamed publication can update it in its one catalog rebuild.
	class SystemCatalogGraphCardinalityCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> nodeCountKey();
		[[nodiscard]] static std::vector<std::byte> edgeCountKey();
		[[nodiscard]] static uint64_t encodeCount(uint64_t count);
		[[nodiscard]] static uint64_t decodeCount(uint64_t encoded);
	};

	constexpr uint64_t RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE = UINT64_C(0x5A5958434154414C);

	class SystemCatalog;

	// A write session is pinned to one NativeGraphWriteTransaction. It batches
	// first-use entries until `finish()` can create the one catalog B+Tree root;
	// later writes update that same root through the transaction-local overlay.
	class SystemCatalogWriteSession final {
	public:
		SystemCatalogWriteSession() = default;
		SystemCatalogWriteSession(const SystemCatalogWriteSession &) = delete;
		SystemCatalogWriteSession &operator=(const SystemCatalogWriteSession &) = delete;
		SystemCatalogWriteSession(SystemCatalogWriteSession &&) noexcept;
		// Mutating the transaction overlay cannot be rolled back by an ordinary
		// move assignment, so replacing an active session is intentionally forbidden.
		SystemCatalogWriteSession &operator=(SystemCatalogWriteSession &&) = delete;
		~SystemCatalogWriteSession() = default;

		[[nodiscard]] uint32_t ensurePropertyToken(std::string_view name);
		[[nodiscard]] uint64_t ensureSchemaToken(SystemCatalogSchemaTokenKind kind, std::string_view name);
		// Definition creation allocates the logical ID and stages all forward,
		// reverse, and semantic directions in this same native transaction.
		[[nodiscard]] IndexDefinition createIndexDefinition(IndexDefinition definition);
		[[nodiscard]] ConstraintDefinition createConstraintDefinition(ConstraintDefinition definition);
		void setConfiguration(std::string_view name, const PropertyValue &value);
		void setGraphCardinalityStatistics(const NativeGraphCardinalityStatistics &statistics);
		void dropIndexDefinition(const IndexDefinition &definition);
		void dropConstraintDefinition(const ConstraintDefinition &definition);
		// Required before committing a session that allocated a previously absent
		// catalog root. It is idempotent and intentionally emits no independent
		// metadata publication.
		void finish();
		[[nodiscard]] bool finished() const { return finished_; }
		[[nodiscard]] bool changed() const { return changed_; }

	private:
		friend class SystemCatalog;
		SystemCatalogWriteSession(SystemCatalog &catalog, NativeGraphWriteTransaction &transaction);

		SystemCatalog *catalog_ = nullptr;
		NativeGraphWriteTransaction *transaction_ = nullptr;
		bool initial_root_ = false;
		bool changed_ = false;
		bool finished_ = false;
		bool property_sequence_loaded_ = false;
		bool schema_sequence_loaded_ = false;
		bool index_sequence_loaded_ = false;
		bool constraint_sequence_loaded_ = false;
		uint64_t next_property_token_ = 0U;
		uint64_t next_schema_token_ = 0U;
		uint64_t next_index_definition_id_ = 0U;
		uint64_t next_constraint_definition_id_ = 0U;
		std::map<std::vector<std::byte>, uint64_t> initial_entries_;
		std::map<std::string, uint32_t, std::less<>> staged_property_tokens_;
		std::map<std::pair<SystemCatalogSchemaTokenKind, std::string>, uint64_t> staged_schema_tokens_;
		std::map<uint64_t, IndexDefinition> staged_index_definitions_;
		std::map<uint64_t, ConstraintDefinition> staged_constraint_definitions_;
		std::map<std::string, PropertyValue, std::less<>> staged_configurations_;
		std::optional<NativeGraphCardinalityStatistics> staged_graph_statistics_;

		void stageUpsert(std::vector<std::byte> key, uint64_t value);
		void stageErase(const std::vector<std::byte> &key);
		[[nodiscard]] uint64_t nextPropertyToken();
		[[nodiscard]] uint64_t nextSchemaToken();
		[[nodiscard]] uint64_t nextIndexDefinitionId();
		[[nodiscard]] uint64_t nextConstraintDefinitionId();
	};

	// One namespaced B+Tree is the authoritative System Catalog root. It keeps
	// property/schema forward and reverse mappings plus durable sequences under
	// one COW root, rather than reviving a group of independently initialized
	// sealed and dynamic registry trees.
	class SystemCatalog final {
	public:
		SystemCatalog(BTreePageStore &pages, RelocatableMetadataStore &metadata);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::optional<uint32_t> findPropertyToken(const MetadataSnapshotPin &snapshot,
																std::string_view name) const;
		[[nodiscard]] std::optional<std::string> findPropertyName(const MetadataSnapshotPin &snapshot,
																  uint32_t token) const;
		[[nodiscard]] std::optional<uint64_t> findSchemaToken(const MetadataSnapshotPin &snapshot,
															  SystemCatalogSchemaTokenKind kind,
															  std::string_view name) const;
		[[nodiscard]] std::optional<SystemCatalogSchemaToken>
		findSchemaToken(const MetadataSnapshotPin &snapshot, SystemCatalogSchemaTokenKind kind, uint64_t token) const;
		[[nodiscard]] std::optional<IndexDefinition> findIndexDefinitionById(const MetadataSnapshotPin &snapshot,
																			 uint64_t indexId) const;
		[[nodiscard]] std::optional<IndexDefinition> findIndexDefinitionByName(const MetadataSnapshotPin &snapshot,
																			   std::string_view name) const;
		[[nodiscard]] std::optional<IndexDefinition>
		findIndexDefinitionBySchema(const MetadataSnapshotPin &snapshot, IndexDefinitionEntityKind entityKind,
									IndexDefinitionKind kind, uint64_t labelTokenId,
									std::span<const std::string> propertyNames, uint32_t vectorDimensions = 0U,
									std::string_view vectorMetric = {}) const;
		[[nodiscard]] std::vector<IndexDefinition> readIndexDefinitions(const MetadataSnapshotPin &snapshot,
																		size_t maximumDefinitions) const;
		[[nodiscard]] std::optional<ConstraintDefinition>
		findConstraintDefinitionById(const MetadataSnapshotPin &snapshot, uint64_t constraintId) const;
		[[nodiscard]] std::optional<ConstraintDefinition>
		findConstraintDefinitionByName(const MetadataSnapshotPin &snapshot, std::string_view name) const;
		[[nodiscard]] std::optional<ConstraintDefinition>
		findConstraintDefinitionBySchema(const MetadataSnapshotPin &snapshot, ConstraintDefinitionEntityKind entityKind,
										 ConstraintDefinitionKind kind, uint64_t schemaTokenId,
										 std::span<const std::string> propertyNames,
										 ConstraintDefinitionPropertyType propertyType) const;
		[[nodiscard]] std::vector<ConstraintDefinition> readConstraintDefinitions(const MetadataSnapshotPin &snapshot,
																				  size_t maximumDefinitions) const;
		[[nodiscard]] std::optional<PropertyValue> findConfiguration(const MetadataSnapshotPin &snapshot,
																	 std::string_view name) const;
		[[nodiscard]] std::vector<NativeConfigurationRecord> readConfigurations(const MetadataSnapshotPin &snapshot,
																				size_t maximumRecords) const;
		// A missing pair denotes a file created before cardinality records existed.
		// A partial pair is corruption and is rejected rather than guessed.
		[[nodiscard]] std::optional<NativeGraphCardinalityStatistics>
		readGraphCardinalityStatistics(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] SystemCatalogWriteSession beginWrite(NativeGraphWriteTransaction &transaction);
		// Internal collaborators use these only to build one transaction-pinned
		// B+Tree overlay; callers still receive logical token APIs above.
		[[nodiscard]] BTreePageStore &pageStore() const { return pages_; }
		[[nodiscard]] RelocatableMetadataStore &metadataStore() const { return metadata_; }

	private:
		friend class SystemCatalogWriteSession;

		BTreePageStore &pages_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
