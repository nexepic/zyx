/**
 * @file SystemCatalog.cpp
 * @brief Canonical native System Catalog for schema and Property tokens.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	namespace {

		// Prefix every key namespace explicitly so a valid mapping cannot be read
		// as a sequence or bootstrap record after storage corruption.
		enum class SystemCatalogKeyKind : uint8_t {
			SCKK_BOOTSTRAP = 0U,
			SCKK_PROPERTY_NAME = 1U,
			SCKK_PROPERTY_TOKEN = 2U,
			SCKK_SCHEMA_NAME = 3U,
			SCKK_SCHEMA_TOKEN = 4U,
			SCKK_SEQUENCE = 5U,
			SCKK_INDEX_DEFINITION = 6U,
			SCKK_INDEX_NAME = 7U,
			SCKK_INDEX_SEMANTIC = 8U,
			SCKK_CONSTRAINT_DEFINITION = 9U,
			SCKK_CONSTRAINT_NAME = 10U,
			SCKK_CONSTRAINT_SEMANTIC = 11U,
			SCKK_CONFIGURATION = 12U,
			SCKK_GRAPH_CARDINALITY = 13U,
		};

		enum class SystemCatalogGraphCardinalityKind : uint8_t {
			SCGCK_NODE_COUNT = 1U,
			SCGCK_EDGE_COUNT = 2U,
		};

		enum class SystemCatalogSequenceKind : uint8_t {
			SCSK_PROPERTY_TOKEN = 1U,
			SCSK_SCHEMA_TOKEN = 2U,
			SCSK_INDEX_DEFINITION = 3U,
			SCSK_CONSTRAINT_DEFINITION = 4U,
		};

		constexpr uint64_t SYSTEM_CATALOG_CONFIGURATION_VALUE = UINT64_C(0x434F4E4649475552);

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateSchemaKind(SystemCatalogSchemaTokenKind kind) {
			switch (kind) {
				case SystemCatalogSchemaTokenKind::SCSTK_LABEL:
				case SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE:
					return;
			}
			fail(FormatError::FERR_INVALID_ENUM, "System Catalog received an unknown schema Token kind");
		}

		void validateName(std::string_view name) {
			if (name.empty() || name.size() > RSS_SYSTEM_CATALOG_NAME_MAX_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog name is outside the canonical persisted byte bound");
			}
		}

		void appendByte(std::vector<std::byte> &target, uint8_t value) {
			target.push_back(static_cast<std::byte>(value));
		}

		void appendBigEndianU32(std::vector<std::byte> &target, uint32_t value) {
			for (uint32_t shift = 24U;; shift -= 8U) {
				appendByte(target, static_cast<uint8_t>((value >> shift) & UINT32_C(0xFF)));
				if (shift == 0U) {
					return;
				}
			}
		}

		void appendBigEndianU64(std::vector<std::byte> &target, uint64_t value) {
			for (uint32_t shift = 56U;; shift -= 8U) {
				appendByte(target, static_cast<uint8_t>((value >> shift) & UINT64_C(0xFF)));
				if (shift == 0U) {
					return;
				}
			}
		}

		[[nodiscard]] uint64_t readBigEndianU64(std::span<const std::byte> bytes, size_t offset) {
			if (offset > bytes.size() || bytes.size() - offset < sizeof(uint64_t)) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "System Catalog definition key is truncated");
			}
			uint64_t value = 0U;
			for (size_t index = 0U; index < sizeof(value); ++index) {
				value = (value << 8U) | std::to_integer<uint8_t>(bytes[offset + index]);
			}
			return value;
		}

		[[nodiscard]] uint32_t readBigEndianU32(std::span<const std::byte> bytes, size_t offset) {
			if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t)) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "System Catalog configuration key is truncated");
			}
			uint32_t value = 0U;
			for (size_t index = 0U; index < sizeof(value); ++index) {
				value = (value << 8U) | std::to_integer<uint8_t>(bytes[offset + index]);
			}
			return value;
		}

		void appendName(std::vector<std::byte> &target, std::string_view name) {
			for (const char character: name) {
				appendByte(target, static_cast<uint8_t>(static_cast<unsigned char>(character)));
			}
		}

		[[nodiscard]] std::vector<std::byte> bootstrapKey() {
			return {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_BOOTSTRAP)};
		}

		[[nodiscard]] std::vector<std::byte> propertyNameKey(std::string_view name) {
			validateName(name);
			std::vector<std::byte> key;
			key.reserve(1U + name.size());
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_PROPERTY_NAME));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> propertyTokenPrefix(uint32_t token) {
			std::vector<std::byte> key;
			key.reserve(1U + sizeof(token));
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_PROPERTY_TOKEN));
			appendBigEndianU32(key, token);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> propertyTokenKey(uint32_t token, std::string_view name) {
			validateName(name);
			std::vector<std::byte> key = propertyTokenPrefix(token);
			key.reserve(key.size() + name.size());
			appendName(key, name);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> schemaNameKey(SystemCatalogSchemaTokenKind kind, std::string_view name) {
			validateSchemaKind(kind);
			validateName(name);
			std::vector<std::byte> key;
			key.reserve(2U + name.size());
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_SCHEMA_NAME));
			appendByte(key, static_cast<uint8_t>(kind));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> schemaTokenPrefix(SystemCatalogSchemaTokenKind kind, uint64_t token) {
			validateSchemaKind(kind);
			std::vector<std::byte> key;
			key.reserve(2U + sizeof(token));
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_SCHEMA_TOKEN));
			appendByte(key, static_cast<uint8_t>(kind));
			appendBigEndianU64(key, token);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token,
															std::string_view name) {
			validateName(name);
			std::vector<std::byte> key = schemaTokenPrefix(kind, token);
			key.reserve(key.size() + name.size());
			appendName(key, name);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> sequenceKey(SystemCatalogSequenceKind kind) {
			return {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_SEQUENCE), static_cast<std::byte>(kind)};
		}

		void validateConfigurationValue(const PropertyValue &value) {
			switch (value.getType()) {
				case PropertyType::BOOLEAN:
				case PropertyType::INTEGER:
				case PropertyType::DOUBLE:
				case PropertyType::STRING:
					return;
				default:
					fail(FormatError::FERR_INVALID_HEADER,
						 "System Catalog configuration value must be a scalar Boolean, Integer, Float, or String");
			}
		}

		[[nodiscard]] std::vector<std::byte> configurationPrefix(std::string_view name) {
			validateName(name);
			std::vector<std::byte> key = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
			appendBigEndianU32(key, static_cast<uint32_t>(name.size()));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> configurationKey(std::string_view name, const PropertyValue &value) {
			validateConfigurationValue(value);
			std::vector<std::byte> key = configurationPrefix(name);
			CanonicalPropertyValueCodec::append(key, value);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> graphCardinalityKey(SystemCatalogGraphCardinalityKind kind) {
			switch (kind) { // this private helper receives only the two fixed kinds below.
				case SystemCatalogGraphCardinalityKind::SCGCK_NODE_COUNT:
				case SystemCatalogGraphCardinalityKind::SCGCK_EDGE_COUNT:
					return {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_GRAPH_CARDINALITY),
							static_cast<std::byte>(kind)};
			}
			fail(FormatError::FERR_INVALID_ENUM, "System Catalog graph cardinality kind is invalid");
		}

		[[nodiscard]] uint64_t encodeGraphCardinalityCount(uint64_t count) {
			if (count == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "System Catalog graph cardinality cannot encode the maximum uint64 value");
			}
			// B+Tree leaves reserve zero as an invalid value. Biasing by one preserves
			// an empty graph without conflating it with a malformed persisted record.
			return count + 1U;
		}

		[[nodiscard]] uint64_t decodeGraphCardinalityCount(uint64_t encoded) {
			if (encoded == 0U) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog graph cardinality record has a zero value");
			}
			return encoded - 1U;
		}

		[[nodiscard]] NativeConfigurationRecord decodeConfigurationKey(std::span<const std::byte> key) {
			const std::vector<std::byte> prefix = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
			if (key.empty() || key.front() != prefix.front()) { // callers filter this exact prefix.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog configuration entry has an invalid key kind");
			}
			const uint32_t nameLength = readBigEndianU32(key, prefix.size());
			const size_t nameOffset = prefix.size() + sizeof(uint32_t);
		if (nameLength == 0U || nameLength > RSS_SYSTEM_CATALOG_NAME_MAX_BYTES ||
			nameLength > key.size() - nameOffset) {
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog configuration entry has an invalid name");
			}
			const std::string name(reinterpret_cast<const char *>(key.data() + nameOffset), nameLength);
			validateName(name);
			size_t valueOffset = nameOffset + nameLength;
			const PropertyValue value = CanonicalPropertyValueCodec::decodeNext(key, valueOffset);
			if (valueOffset != key.size()) {
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog configuration entry has trailing bytes");
			}
			validateConfigurationValue(value);
			return {name, value};
		}

		[[nodiscard]] bool isIndexDefinitionId(uint64_t id) {
			return id >= RSS_SYSTEM_CATALOG_INDEX_ID_FIRST && id <= RSS_SYSTEM_CATALOG_INDEX_ID_LAST;
		}

		[[nodiscard]] bool isConstraintDefinitionId(uint64_t id) {
			return id >= RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST && id <= RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST;
		}

		[[nodiscard]] std::vector<std::byte> definitionPrefix(SystemCatalogKeyKind kind) {
			return {static_cast<std::byte>(kind)};
		}

		[[nodiscard]] bool startsWith(std::span<const std::byte> bytes, std::span<const std::byte> prefix);

		[[nodiscard]] std::vector<std::byte> indexDefinitionLowerBound(uint64_t id) {
			if (!isIndexDefinitionId(id)) { // callers validate IDs before this private encoder.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition ID is outside its namespace");
			}
			std::vector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
			appendBigEndianU64(key, id);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> constraintDefinitionLowerBound(uint64_t id) {
			if (!isConstraintDefinitionId(id)) { // callers validate IDs before this private encoder.
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog constraint definition ID is outside its namespace");
			}
			std::vector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
			appendBigEndianU64(key, id);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> indexDefinitionKey(const IndexDefinition &definition) {
			const std::vector<std::byte> encoded = IndexDefinitionCodec::encode(definition);
			std::vector<std::byte> key = indexDefinitionLowerBound(definition.index_id);
			key.insert(key.end(), encoded.begin(), encoded.end());
			return key;
		}

		[[nodiscard]] std::vector<std::byte> constraintDefinitionKey(const ConstraintDefinition &definition) {
			const std::vector<std::byte> encoded = ConstraintDefinitionCodec::encode(definition);
			std::vector<std::byte> key = constraintDefinitionLowerBound(definition.constraint_id);
			key.insert(key.end(), encoded.begin(), encoded.end());
			return key;
		}

		[[nodiscard]] std::vector<std::byte> namedKey(SystemCatalogKeyKind kind, std::string_view name) {
			validateName(name);
			std::vector<std::byte> key = definitionPrefix(kind);
			appendName(key, name);
			return key;
		}

		void appendLengthPrefixedString(std::vector<std::byte> &key, std::string_view value) {
			if (value.empty() || value.size() > std::numeric_limits<uint16_t>::max()) { // codec validates it.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog semantic key contains an invalid string");
			}
			key.push_back(static_cast<std::byte>((value.size() >> 8U) & UINT16_C(0xFF)));
			key.push_back(static_cast<std::byte>(value.size() & UINT16_C(0xFF)));
			appendName(key, value);
		}

		[[nodiscard]] std::vector<std::byte> indexSemanticKey(const IndexDefinition &definition) {
			std::vector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_SEMANTIC);
			appendByte(key, static_cast<uint8_t>(definition.entity_kind));
			appendByte(key, static_cast<uint8_t>(definition.kind));
			appendBigEndianU64(key, definition.label_token_id);
			appendBigEndianU32(key, definition.vector_dimensions);
			if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
				appendLengthPrefixedString(key, definition.vector_metric);
			} else {
				key.push_back(std::byte{0U});
				key.push_back(std::byte{0U});
			}
			if (definition.property_names.size() > RSS_INDEX_DEFINITION_MAX_PROPERTIES) { // codec validates it.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index semantic key has too many properties");
			}
			key.push_back(static_cast<std::byte>((definition.property_names.size() >> 8U) & UINT16_C(0xFF)));
			key.push_back(static_cast<std::byte>(definition.property_names.size() & UINT16_C(0xFF)));
			for (const std::string &property: definition.property_names) {
				appendLengthPrefixedString(key, property);
			}
			return key;
		}

		[[nodiscard]] std::vector<std::byte> constraintSemanticKey(const ConstraintDefinition &definition) {
			std::vector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_SEMANTIC);
			appendByte(key, static_cast<uint8_t>(definition.entity_kind));
			appendByte(key, static_cast<uint8_t>(definition.kind));
			appendBigEndianU64(key, definition.schema_token_id);
			appendByte(key, static_cast<uint8_t>(definition.property_type));
			if (definition.property_names.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES) { // codec validates it.
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog constraint semantic key has too many properties");
			}
			key.push_back(static_cast<std::byte>((definition.property_names.size() >> 8U) & UINT16_C(0xFF)));
			key.push_back(static_cast<std::byte>(definition.property_names.size() & UINT16_C(0xFF)));
			for (const std::string &property: definition.property_names) {
				appendLengthPrefixedString(key, property);
			}
			return key;
		}

		[[nodiscard]] IndexDefinition decodeIndexDefinition(std::span<const std::byte> key) {
			const std::vector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
			if (!startsWith(key, prefix)) {
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition entry has an invalid key kind");
			}
			const uint64_t id = readBigEndianU64(key, prefix.size());
			if (key.size() == prefix.size() + sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition entry has no encoded payload");
			}
			const IndexDefinition definition =
					IndexDefinitionCodec::decode(key.subspan(prefix.size() + sizeof(uint64_t)));
			if (definition.index_id != id || !isIndexDefinitionId(id)) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition key disagrees with its value");
			}
			return definition;
		}

		[[nodiscard]] ConstraintDefinition decodeConstraintDefinition(std::span<const std::byte> key) {
			const std::vector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
			if (!startsWith(key, prefix)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog constraint definition entry has an invalid key kind");
			}
			const uint64_t id = readBigEndianU64(key, prefix.size());
			if (key.size() == prefix.size() + sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog constraint definition entry has no encoded payload");
			}
			const ConstraintDefinition definition =
					ConstraintDefinitionCodec::decode(key.subspan(prefix.size() + sizeof(uint64_t)));
			if (definition.constraint_id != id || !isConstraintDefinitionId(id)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "System Catalog constraint definition key disagrees with its value");
			}
			return definition;
		}

		[[nodiscard]] bool startsWith(std::span<const std::byte> bytes, std::span<const std::byte> prefix) {
			return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
		}

		void verifyBootstrap(const SystemCatalog &catalog, const MetadataSnapshotPin &snapshot) {
			const std::optional<uint64_t> value =
					BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore())
							.find(snapshot, bootstrapKey());
			if (!value.has_value() || *value != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
			}
		}

		[[nodiscard]] bool hasRoot(const SystemCatalog &catalog, const MetadataSnapshotPin &snapshot) {
			return catalog.metadataStore().findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value();
		}

		[[nodiscard]] std::optional<uint64_t> readSequence(const SystemCatalog &catalog,
														   const MetadataSnapshotPin &snapshot,
														   SystemCatalogSequenceKind kind) {
			if (!hasRoot(catalog, snapshot)) {
				return std::nullopt;
			}
			verifyBootstrap(catalog, snapshot);
			return BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore())
					.find(snapshot, sequenceKey(kind));
		}

		[[nodiscard]] std::optional<std::string> findReverseName(const SystemCatalog &catalog,
																 const MetadataSnapshotPin &snapshot,
																 std::span<const std::byte> prefix,
																 uint64_t expectedValue) {
			const std::vector<BTreePageEntry> entries =
					BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore())
							.scan(snapshot, prefix, 2U);
			if (entries.empty() || !startsWith(entries.front().key, prefix)) {
				return std::nullopt;
			}
			if (entries.front().value_or_child != expectedValue || entries.front().key.size() == prefix.size() ||
				(entries.size() == 2U && startsWith(entries[1].key, prefix))) {
				fail(FormatError::FERR_INVALID_STATE,
					 "System Catalog reverse mapping is malformed, ambiguous, or mismatched");
			}
			const std::string name(reinterpret_cast<const char *>(entries.front().key.data() + prefix.size()),
								   entries.front().key.size() - prefix.size());
			validateName(name);
			return name;
		}

	} // namespace

	std::vector<std::byte> SystemCatalogTokenKeyCodec::bootstrapKey() { return ::graph::storage::bootstrapKey(); }

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyNameKey(std::string_view name) {
		return ::graph::storage::propertyNameKey(name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenKey(uint32_t token, std::string_view name) {
		return ::graph::storage::propertyTokenKey(token, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaNameKey(SystemCatalogSchemaTokenKind kind,
																	 std::string_view name) {
		return ::graph::storage::schemaNameKey(kind, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token,
																	  std::string_view name) {
		return ::graph::storage::schemaTokenKey(kind, token, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenSequenceKey() {
		return ::graph::storage::sequenceKey(SystemCatalogSequenceKind::SCSK_PROPERTY_TOKEN);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenSequenceKey() {
		return ::graph::storage::sequenceKey(SystemCatalogSequenceKind::SCSK_SCHEMA_TOKEN);
	}

	void SystemCatalogTokenKeyCodec::validateName(std::string_view name) { ::graph::storage::validateName(name); }

	void SystemCatalogTokenKeyCodec::validateSchemaKind(SystemCatalogSchemaTokenKind kind) {
		::graph::storage::validateSchemaKind(kind);
	}

	std::vector<std::byte> SystemCatalogGraphCardinalityCodec::nodeCountKey() {
		return ::graph::storage::graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_NODE_COUNT);
	}

	std::vector<std::byte> SystemCatalogGraphCardinalityCodec::edgeCountKey() {
		return ::graph::storage::graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_EDGE_COUNT);
	}

	uint64_t SystemCatalogGraphCardinalityCodec::encodeCount(uint64_t count) {
		return ::graph::storage::encodeGraphCardinalityCount(count);
	}

	uint64_t SystemCatalogGraphCardinalityCodec::decodeCount(uint64_t encoded) {
		return ::graph::storage::decodeGraphCardinalityCount(encoded);
	}

	SystemCatalogWriteSession::SystemCatalogWriteSession(SystemCatalog &catalog,
														 NativeGraphWriteTransaction &transaction) :
		catalog_(&catalog), transaction_(&transaction), initial_root_(!hasRoot(catalog, transaction.baseSnapshot())) {
		if (initial_root_) {
			initial_entries_.emplace(bootstrapKey(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE);
			initial_entries_.emplace(graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_NODE_COUNT), 1U);
			initial_entries_.emplace(graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_EDGE_COUNT), 1U);
		} else {
			verifyBootstrap(catalog, transaction.baseSnapshot());
		}
	}

	SystemCatalogWriteSession::SystemCatalogWriteSession(SystemCatalogWriteSession &&other) noexcept :
		catalog_(std::exchange(other.catalog_, nullptr)), transaction_(std::exchange(other.transaction_, nullptr)),
		initial_root_(std::exchange(other.initial_root_, false)), changed_(std::exchange(other.changed_, false)),
		finished_(std::exchange(other.finished_, false)),
		property_sequence_loaded_(std::exchange(other.property_sequence_loaded_, false)),
		schema_sequence_loaded_(std::exchange(other.schema_sequence_loaded_, false)),
		index_sequence_loaded_(std::exchange(other.index_sequence_loaded_, false)),
		constraint_sequence_loaded_(std::exchange(other.constraint_sequence_loaded_, false)),
		next_property_token_(std::exchange(other.next_property_token_, 0U)),
		next_schema_token_(std::exchange(other.next_schema_token_, 0U)),
		next_index_definition_id_(std::exchange(other.next_index_definition_id_, 0U)),
		next_constraint_definition_id_(std::exchange(other.next_constraint_definition_id_, 0U)),
		initial_entries_(std::move(other.initial_entries_)),
		staged_property_tokens_(std::move(other.staged_property_tokens_)),
		staged_schema_tokens_(std::move(other.staged_schema_tokens_)),
		staged_index_definitions_(std::move(other.staged_index_definitions_)),
		staged_constraint_definitions_(std::move(other.staged_constraint_definitions_)),
		staged_configurations_(std::move(other.staged_configurations_)),
		staged_graph_statistics_(std::move(other.staged_graph_statistics_)) {}

	void SystemCatalogWriteSession::stageUpsert(std::vector<std::byte> key, uint64_t value) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		changed_ = true;
		if (initial_root_) {
			initial_entries_.insert_or_assign(std::move(key), value);
			return;
		}
		transaction_->stageScalarIndexUpsert(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID, key, value);
	}

	void SystemCatalogWriteSession::stageErase(const std::vector<std::byte> &key) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		changed_ = true;
		if (initial_root_) {
			initial_entries_.erase(key);
			return;
		}
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID, key);
	}

	uint64_t SystemCatalogWriteSession::nextPropertyToken() {
		if (!property_sequence_loaded_) {
			const std::optional<uint64_t> stored = readSequence(*catalog_, transaction_->baseSnapshot(),
																SystemCatalogSequenceKind::SCSK_PROPERTY_TOKEN);
			next_property_token_ = stored.value_or(1U);
			property_sequence_loaded_ = true;
		}
		if (next_property_token_ == 0U || next_property_token_ > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "System Catalog exhausted Property-key Token IDs");
		}
		const uint64_t token = next_property_token_++;
		stageUpsert(sequenceKey(SystemCatalogSequenceKind::SCSK_PROPERTY_TOKEN), next_property_token_);
		return token;
	}

	uint64_t SystemCatalogWriteSession::nextSchemaToken() {
		if (!schema_sequence_loaded_) {
			const std::optional<uint64_t> stored =
					readSequence(*catalog_, transaction_->baseSnapshot(), SystemCatalogSequenceKind::SCSK_SCHEMA_TOKEN);
			next_schema_token_ = stored.value_or(1U);
			schema_sequence_loaded_ = true;
		}
		if (next_schema_token_ == 0U) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "System Catalog exhausted schema Token IDs");
		}
		const uint64_t token = next_schema_token_;
		next_schema_token_ = token == std::numeric_limits<uint64_t>::max() ? 0U : token + 1U;
		stageUpsert(sequenceKey(SystemCatalogSequenceKind::SCSK_SCHEMA_TOKEN), next_schema_token_);
		return token;
	}

	uint64_t SystemCatalogWriteSession::nextIndexDefinitionId() {
		if (!index_sequence_loaded_) {
			next_index_definition_id_ = readSequence(*catalog_, transaction_->baseSnapshot(),
													 SystemCatalogSequenceKind::SCSK_INDEX_DEFINITION)
												.value_or(RSS_SYSTEM_CATALOG_INDEX_ID_FIRST);
			index_sequence_loaded_ = true;
		}
		if (!isIndexDefinitionId(next_index_definition_id_)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "System Catalog exhausted Index definition IDs");
		}
		const uint64_t allocated = next_index_definition_id_;
		next_index_definition_id_ = allocated == RSS_SYSTEM_CATALOG_INDEX_ID_LAST ? 0U : allocated + 1U;
		stageUpsert(sequenceKey(SystemCatalogSequenceKind::SCSK_INDEX_DEFINITION), next_index_definition_id_);
		return allocated;
	}

	uint64_t SystemCatalogWriteSession::nextConstraintDefinitionId() {
		if (!constraint_sequence_loaded_) {
			next_constraint_definition_id_ = readSequence(*catalog_, transaction_->baseSnapshot(),
														  SystemCatalogSequenceKind::SCSK_CONSTRAINT_DEFINITION)
													 .value_or(RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST);
			constraint_sequence_loaded_ = true;
		}
		if (!isConstraintDefinitionId(next_constraint_definition_id_)) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "System Catalog exhausted constraint definition IDs");
		}
		const uint64_t allocated = next_constraint_definition_id_;
		next_constraint_definition_id_ = allocated == RSS_SYSTEM_CATALOG_CONSTRAINT_ID_LAST ? 0U : allocated + 1U;
		stageUpsert(sequenceKey(SystemCatalogSequenceKind::SCSK_CONSTRAINT_DEFINITION), next_constraint_definition_id_);
		return allocated;
	}

	uint32_t SystemCatalogWriteSession::ensurePropertyToken(std::string_view name) {
		validateName(name);
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const auto staged = staged_property_tokens_.find(name);
		if (staged != staged_property_tokens_.end()) {
			return staged->second;
		}
		if (const std::optional<uint32_t> existing = catalog_->findPropertyToken(transaction_->baseSnapshot(), name);
			existing.has_value()) {
			staged_property_tokens_.emplace(std::string(name), *existing);
			return *existing;
		}
		const uint64_t allocated = nextPropertyToken();
		const uint32_t token = static_cast<uint32_t>(allocated);
		stageUpsert(propertyNameKey(name), token);
		stageUpsert(propertyTokenKey(token, name), token);
		staged_property_tokens_.emplace(std::string(name), token);
		return token;
	}

	uint64_t SystemCatalogWriteSession::ensureSchemaToken(SystemCatalogSchemaTokenKind kind, std::string_view name) {
		validateSchemaKind(kind);
		validateName(name);
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const auto staged = staged_schema_tokens_.find({kind, std::string(name)});
		if (staged != staged_schema_tokens_.end()) {
			return staged->second;
		}
		if (const std::optional<uint64_t> existing =
					catalog_->findSchemaToken(transaction_->baseSnapshot(), kind, name);
			existing.has_value()) {
			staged_schema_tokens_.emplace(std::make_pair(kind, std::string(name)), *existing);
			return *existing;
		}
		const uint64_t token = nextSchemaToken();
		stageUpsert(schemaNameKey(kind, name), token);
		stageUpsert(schemaTokenKey(kind, token, name), token);
		staged_schema_tokens_.emplace(std::make_pair(kind, std::string(name)), token);
		return token;
	}

	IndexDefinition SystemCatalogWriteSession::createIndexDefinition(IndexDefinition definition) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		if (definition.index_id != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog allocates Index definition IDs");
		}
		// Validate and reject duplicate definitions before staging a durable sequence
		// advance. A caller may recover from validation failure and commit other work.
		definition.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
		static_cast<void>(IndexDefinitionCodec::encode(definition));
		if (std::ranges::any_of(staged_index_definitions_, [&](const auto &entry) {
				return entry.second.name == definition.name ||
					   indexSemanticKey(entry.second) == indexSemanticKey(definition);
			})) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition duplicates a staged name or schema");
		}
		if (catalog_->findIndexDefinitionByName(transaction_->baseSnapshot(), definition.name).has_value() ||
			catalog_->findIndexDefinitionBySchema(transaction_->baseSnapshot(), definition.entity_kind, definition.kind,
												  definition.label_token_id, definition.property_names,
												  definition.vector_dimensions, definition.vector_metric)
					.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog Index definition duplicates a durable name or schema");
		}
		definition.index_id = nextIndexDefinitionId();
		stageUpsert(indexDefinitionKey(definition), definition.index_id);
		stageUpsert(namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, definition.name), definition.index_id);
		stageUpsert(indexSemanticKey(definition), definition.index_id);
		staged_index_definitions_.emplace(definition.index_id, definition);
		return definition;
	}

	ConstraintDefinition SystemCatalogWriteSession::createConstraintDefinition(ConstraintDefinition definition) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		if (definition.constraint_id != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog allocates constraint definition IDs");
		}
		// As with Index definitions, leave the transaction overlay untouched until
		// validation and duplicate checks have succeeded.
		definition.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		static_cast<void>(ConstraintDefinitionCodec::encode(definition));
		if (std::ranges::any_of(staged_constraint_definitions_, [&](const auto &entry) {
				return entry.second.name == definition.name ||
					   constraintSemanticKey(entry.second) == constraintSemanticKey(definition);
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog constraint definition duplicates a staged name or schema");
		}
		if (catalog_->findConstraintDefinitionByName(transaction_->baseSnapshot(), definition.name).has_value() ||
			catalog_->findConstraintDefinitionBySchema(transaction_->baseSnapshot(), definition.entity_kind,
													   definition.kind, definition.schema_token_id,
													   definition.property_names, definition.property_type)
					.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog constraint definition duplicates a durable name or schema");
		}
		definition.constraint_id = nextConstraintDefinitionId();
		stageUpsert(constraintDefinitionKey(definition), definition.constraint_id);
		stageUpsert(namedKey(SystemCatalogKeyKind::SCKK_CONSTRAINT_NAME, definition.name), definition.constraint_id);
		stageUpsert(constraintSemanticKey(definition), definition.constraint_id);
		staged_constraint_definitions_.emplace(definition.constraint_id, definition);
		return definition;
	}

	void SystemCatalogWriteSession::setConfiguration(std::string_view name, const PropertyValue &value) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const std::string ownedName(name);
		static_cast<void>(configurationKey(ownedName, value));
		if (const auto staged = staged_configurations_.find(ownedName); staged != staged_configurations_.end()) {
			stageErase(configurationKey(ownedName, staged->second));
		} else if (const std::optional<PropertyValue> current =
						   catalog_->findConfiguration(transaction_->baseSnapshot(), ownedName);
				   current.has_value()) {
			stageErase(configurationKey(ownedName, *current));
		}
		stageUpsert(configurationKey(ownedName, value), SYSTEM_CATALOG_CONFIGURATION_VALUE);
		staged_configurations_.insert_or_assign(std::move(ownedName), value);
	}

	void SystemCatalogWriteSession::setGraphCardinalityStatistics(const NativeGraphCardinalityStatistics &statistics) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		stageUpsert(SystemCatalogGraphCardinalityCodec::nodeCountKey(),
					SystemCatalogGraphCardinalityCodec::encodeCount(statistics.node_count));
		stageUpsert(SystemCatalogGraphCardinalityCodec::edgeCountKey(),
					SystemCatalogGraphCardinalityCodec::encodeCount(statistics.edge_count));
		staged_graph_statistics_ = statistics;
	}

	void SystemCatalogWriteSession::dropIndexDefinition(const IndexDefinition &definition) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const std::optional<IndexDefinition> current =
				catalog_->findIndexDefinitionById(transaction_->baseSnapshot(), definition.index_id);
		if (!current.has_value() || *current != definition) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition drop does not match its snapshot");
		}
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											indexDefinitionKey(definition));
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, definition.name));
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											indexSemanticKey(definition));
		changed_ = true;
	}

	void SystemCatalogWriteSession::dropConstraintDefinition(const ConstraintDefinition &definition) {
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const std::optional<ConstraintDefinition> current =
				catalog_->findConstraintDefinitionById(transaction_->baseSnapshot(), definition.constraint_id);
		if (!current.has_value() || *current != definition) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog constraint definition drop does not match its snapshot");
		}
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											constraintDefinitionKey(definition));
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											namedKey(SystemCatalogKeyKind::SCKK_CONSTRAINT_NAME, definition.name));
		transaction_->stageScalarIndexErase(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID,
											constraintSemanticKey(definition));
		changed_ = true;
	}

	void SystemCatalogWriteSession::finish() {
		if (catalog_ == nullptr || transaction_ == nullptr) {
			throw std::logic_error("System Catalog write session is detached");
		}
		if (finished_) {
			return;
		}
		if (initial_root_ && changed_) {
			std::vector<BTreePageEntry> entries;
			entries.reserve(initial_entries_.size());
			for (const auto &[key, value]: initial_entries_) {
				entries.push_back({key, value});
			}
			transaction_->stageScalarIndexCreate(catalog_->pages_, RSS_SYSTEM_CATALOG_INDEX_ID, entries);
		}
		finished_ = true;
	}

	SystemCatalog::SystemCatalog(BTreePageStore &pages, RelocatableMetadataStore &metadata) :
		pages_(pages), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument("System Catalog requires an Index store in its metadata domain");
		}
	}

	std::optional<uint32_t> SystemCatalog::findPropertyToken(const MetadataSnapshotPin &snapshot,
															 std::string_view name) const {
		validateName(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> value =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, propertyNameKey(name));
		if (!value.has_value()) {
			return std::nullopt;
		}
		if (*value == 0U || *value > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Property-key mapping has an invalid Token");
		}
		const uint32_t token = static_cast<uint32_t>(*value);
		const std::optional<std::string> reverse = findReverseName(*this, snapshot, propertyTokenPrefix(token), token);
		if (!reverse.has_value() || *reverse != name) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog Property-key forward mapping has no matching reverse mapping");
		}
		return token;
	}

	std::optional<NativeGraphCardinalityStatistics>
	SystemCatalog::readGraphCardinalityStatistics(const MetadataSnapshotPin &snapshot) const {
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
		const std::optional<uint64_t> nodes = index.find(snapshot, SystemCatalogGraphCardinalityCodec::nodeCountKey());
		const std::optional<uint64_t> edges = index.find(snapshot, SystemCatalogGraphCardinalityCodec::edgeCountKey());
		if (nodes.has_value() != edges.has_value()) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog graph cardinality record is incomplete");
		}
		if (!nodes.has_value()) {
			return std::nullopt;
		}
		return NativeGraphCardinalityStatistics{SystemCatalogGraphCardinalityCodec::decodeCount(*nodes),
												SystemCatalogGraphCardinalityCodec::decodeCount(*edges)};
	}

	std::optional<std::string> SystemCatalog::findPropertyName(const MetadataSnapshotPin &snapshot,
															   uint32_t token) const {
		if (token == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog Property-key lookup requires a nonzero Token");
		}
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<std::string> name = findReverseName(*this, snapshot, propertyTokenPrefix(token), token);
		if (!name.has_value()) {
			return std::nullopt;
		}
		const std::optional<uint64_t> forward =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, propertyNameKey(*name));
		if (!forward.has_value() || *forward != token) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog Property-key reverse mapping has no matching forward mapping");
		}
		return name;
	}

	std::optional<uint64_t> SystemCatalog::findSchemaToken(const MetadataSnapshotPin &snapshot,
														   SystemCatalogSchemaTokenKind kind,
														   std::string_view name) const {
		validateSchemaKind(kind);
		validateName(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> value =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, schemaNameKey(kind, name));
		if (!value.has_value()) {
			return std::nullopt;
		}
		if (*value == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog schema mapping has an invalid Token");
		}
		const std::optional<SystemCatalogSchemaToken> reverse = findSchemaToken(snapshot, kind, *value);
		if (!reverse.has_value() || reverse->name != name) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog schema forward mapping has no matching reverse mapping");
		}
		return *value;
	}

	std::optional<SystemCatalogSchemaToken> SystemCatalog::findSchemaToken(const MetadataSnapshotPin &snapshot,
																		   SystemCatalogSchemaTokenKind kind,
																		   uint64_t token) const {
		validateSchemaKind(kind);
		if (token == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog schema lookup requires a nonzero Token");
		}
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<std::string> name = findReverseName(*this, snapshot, schemaTokenPrefix(kind, token), token);
		if (!name.has_value()) {
			return std::nullopt;
		}
		const std::optional<uint64_t> forward =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, schemaNameKey(kind, *name));
		if (!forward.has_value() || *forward != token) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog schema reverse mapping has no matching forward mapping");
		}
		return SystemCatalogSchemaToken{token, kind, *name};
	}

	std::optional<IndexDefinition> SystemCatalog::findIndexDefinitionById(const MetadataSnapshotPin &snapshot,
																		  uint64_t indexId) const {
		if (indexId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition lookup requires a nonzero ID");
		}
		if (!isIndexDefinitionId(indexId) || !hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<std::byte> lower = indexDefinitionLowerBound(indexId);
		const std::vector<BTreePageEntry> entries =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).scan(snapshot, lower, 2U);
		if (entries.empty() || !startsWith(entries.front().key, lower)) {
			return std::nullopt;
		}
		if (entries.size() == 2U && startsWith(entries[1].key, lower)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog contains multiple Index definitions for one logical ID");
		}
		const IndexDefinition definition = decodeIndexDefinition(entries.front().key);
		if (entries.front().value_or_child != indexId) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition value disagrees with its key");
		}
		const std::optional<uint64_t> byName =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
						.find(snapshot, namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, definition.name));
		const std::optional<uint64_t> bySemantic =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, indexSemanticKey(definition));
		if (!byName.has_value() || !bySemantic.has_value() || *byName != indexId || *bySemantic != indexId) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition directions do not agree");
		}
		return definition;
	}

	std::optional<IndexDefinition> SystemCatalog::findIndexDefinitionByName(const MetadataSnapshotPin &snapshot,
																			std::string_view name) const {
		validateName(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> id =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
						.find(snapshot, namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, name));
		if (!id.has_value()) {
			return std::nullopt;
		}
		const std::optional<IndexDefinition> definition = findIndexDefinitionById(snapshot, *id);
		if (!definition.has_value() || definition->name != name) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index name direction has no matching definition");
		}
		return definition;
	}

	std::optional<IndexDefinition>
	SystemCatalog::findIndexDefinitionBySchema(const MetadataSnapshotPin &snapshot,
											   IndexDefinitionEntityKind entityKind, IndexDefinitionKind kind,
											   uint64_t labelTokenId, std::span<const std::string> propertyNames,
											   uint32_t vectorDimensions, std::string_view vectorMetric) const {
		IndexDefinition probe;
		probe.name = "_";
		probe.entity_kind = entityKind;
		probe.kind = kind;
		probe.label_token_id = labelTokenId;
		probe.property_names.assign(propertyNames.begin(), propertyNames.end());
		probe.vector_dimensions = vectorDimensions;
		probe.vector_metric = vectorMetric;
		probe.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
		static_cast<void>(IndexDefinitionCodec::encode(probe));
		const std::vector<std::byte> key = indexSemanticKey(probe);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> id =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
		if (!id.has_value()) {
			return std::nullopt;
		}
		const std::optional<IndexDefinition> definition = findIndexDefinitionById(snapshot, *id);
		if (!definition.has_value() || indexSemanticKey(*definition) != key) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog Index semantic direction has no matching definition");
		}
		return definition;
	}

	std::vector<IndexDefinition> SystemCatalog::readIndexDefinitions(const MetadataSnapshotPin &snapshot,
																	 size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U || maximumDefinitions == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog Index definition read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
		const std::vector<BTreePageEntry> entries = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
															.scan(snapshot, prefix, maximumDefinitions + 1U);
		std::vector<IndexDefinition> result;
		for (const BTreePageEntry &entry: entries) {
			if (!startsWith(entry.key, prefix)) {
				break;
			}
			if (result.size() == maximumDefinitions) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definitions exceed the caller bound");
			}
			const IndexDefinition definition = decodeIndexDefinition(entry.key);
			if (entry.value_or_child != definition.index_id ||
				!findIndexDefinitionById(snapshot, definition.index_id).has_value()) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition is not self-consistent");
			}
			result.push_back(definition);
		}
		return result;
	}

	std::optional<ConstraintDefinition> SystemCatalog::findConstraintDefinitionById(const MetadataSnapshotPin &snapshot,
																					uint64_t constraintId) const {
		if (constraintId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog constraint definition lookup requires a nonzero ID");
		}
		if (!isConstraintDefinitionId(constraintId) || !hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<std::byte> lower = constraintDefinitionLowerBound(constraintId);
		const std::vector<BTreePageEntry> entries =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).scan(snapshot, lower, 2U);
		if (entries.empty() || !startsWith(entries.front().key, lower)) {
			return std::nullopt;
		}
		if (entries.size() == 2U && startsWith(entries[1].key, lower)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog contains multiple constraint definitions for one logical ID");
		}
		const ConstraintDefinition definition = decodeConstraintDefinition(entries.front().key);
		if (entries.front().value_or_child != constraintId) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog constraint definition value disagrees with its key");
		}
		const std::optional<uint64_t> byName =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
						.find(snapshot, namedKey(SystemCatalogKeyKind::SCKK_CONSTRAINT_NAME, definition.name));
		const std::optional<uint64_t> bySemantic = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
														   .find(snapshot, constraintSemanticKey(definition));
		if (!byName.has_value() || !bySemantic.has_value() || *byName != constraintId || *bySemantic != constraintId) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog constraint definition directions do not agree");
		}
		return definition;
	}

	std::optional<ConstraintDefinition>
	SystemCatalog::findConstraintDefinitionByName(const MetadataSnapshotPin &snapshot, std::string_view name) const {
		validateName(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> id =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
						.find(snapshot, namedKey(SystemCatalogKeyKind::SCKK_CONSTRAINT_NAME, name));
		if (!id.has_value()) {
			return std::nullopt;
		}
		const std::optional<ConstraintDefinition> definition = findConstraintDefinitionById(snapshot, *id);
		if (!definition.has_value() || definition->name != name) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog constraint name direction has no matching definition");
		}
		return definition;
	}

	std::optional<ConstraintDefinition> SystemCatalog::findConstraintDefinitionBySchema(
			const MetadataSnapshotPin &snapshot, ConstraintDefinitionEntityKind entityKind,
			ConstraintDefinitionKind kind, uint64_t schemaTokenId, std::span<const std::string> propertyNames,
			ConstraintDefinitionPropertyType propertyType) const {
		ConstraintDefinition probe;
		probe.name = "_";
		probe.entity_kind = entityKind;
		probe.kind = kind;
		probe.schema_token_id = schemaTokenId;
		probe.property_names.assign(propertyNames.begin(), propertyNames.end());
		probe.property_type = propertyType;
		probe.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		static_cast<void>(ConstraintDefinitionCodec::encode(probe));
		const std::vector<std::byte> key = constraintSemanticKey(probe);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::optional<uint64_t> id =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
		if (!id.has_value()) {
			return std::nullopt;
		}
		const std::optional<ConstraintDefinition> definition = findConstraintDefinitionById(snapshot, *id);
		if (!definition.has_value() || constraintSemanticKey(*definition) != key) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog constraint semantic direction has no matching definition");
		}
		return definition;
	}

	std::vector<ConstraintDefinition> SystemCatalog::readConstraintDefinitions(const MetadataSnapshotPin &snapshot,
																			   size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U || maximumDefinitions == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog constraint definition read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
		const std::vector<BTreePageEntry> entries = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_)
															.scan(snapshot, prefix, maximumDefinitions + 1U);
		std::vector<ConstraintDefinition> result;
		for (const BTreePageEntry &entry: entries) {
			if (!startsWith(entry.key, prefix)) {
				break;
			}
			if (result.size() == maximumDefinitions) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog constraint definitions exceed the caller bound");
			}
			const ConstraintDefinition definition = decodeConstraintDefinition(entry.key);
			if (entry.value_or_child != definition.constraint_id ||
				!findConstraintDefinitionById(snapshot, definition.constraint_id).has_value()) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog constraint definition is not self-consistent");
			}
			result.push_back(definition);
		}
		return result;
	}

	std::optional<PropertyValue> SystemCatalog::findConfiguration(const MetadataSnapshotPin &snapshot,
																  std::string_view name) const {
		const std::vector<std::byte> prefix = configurationPrefix(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<BTreePageEntry> entries =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).scan(snapshot, prefix, 2U);
		if (entries.empty() || !startsWith(entries.front().key, prefix)) {
			return std::nullopt;
		}
		if (entries.size() == 2U && startsWith(entries[1].key, prefix)) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog contains multiple values for one configuration name");
		}
		if (entries.front().value_or_child != SYSTEM_CATALOG_CONFIGURATION_VALUE) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog configuration entry has an invalid value marker");
		}
		const NativeConfigurationRecord record = decodeConfigurationKey(entries.front().key);
		if (record.name != name) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog configuration entry disagrees with its name prefix");
		}
		return record.value;
	}

	std::vector<NativeConfigurationRecord> SystemCatalog::readConfigurations(const MetadataSnapshotPin &snapshot,
																			 size_t maximumRecords) const {
		if (maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog configuration read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const std::vector<std::byte> prefix = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
		const std::vector<BTreePageEntry> entries =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).scan(snapshot, prefix, maximumRecords + 1U);
		std::set<std::string, std::less<>> names;
		std::vector<NativeConfigurationRecord> result;
		for (const BTreePageEntry &entry: entries) {
			if (!startsWith(entry.key, prefix)) {
				break;
			}
			if (result.size() == maximumRecords) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog configurations exceed the caller bound");
			}
			if (entry.value_or_child != SYSTEM_CATALOG_CONFIGURATION_VALUE) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog configuration entry has an invalid value marker");
			}
			NativeConfigurationRecord record = decodeConfigurationKey(entry.key);
			if (!names.emplace(record.name).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "System Catalog contains multiple values for one configuration name");
			}
			result.push_back(std::move(record));
		}
		std::ranges::sort(result, {}, &NativeConfigurationRecord::name);
		return result;
	}

	SystemCatalogWriteSession SystemCatalog::beginWrite(NativeGraphWriteTransaction &transaction) {
		return SystemCatalogWriteSession(*this, transaction);
	}

} // namespace graph::storage
