/**
 * @file SystemCatalog.cpp
 * @brief Canonical native System Catalog for schema and Property tokens.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/detail/ScopedPropertyBuffers.hpp"

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
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	namespace {

        std::shared_ptr<std::pmr::memory_resource> schemaAllocationOwner(const MetadataSnapshotPin &snapshot) {
            if (utils::OwnedAllocationScope::active()) return utils::OwnedAllocationScope::owner();
            return snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr;
        }

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

        [[nodiscard]] bool catalogOwnedHeapAdmission() noexcept {
            return dynamic_cast<utils::UnmanagedAllocationResource *>(utils::OwnedAllocationScope::owner().get()) != nullptr;
        }

		void appendByte(utils::OwnedStdVector<std::byte> &target, uint8_t value) {
			target.push_back(static_cast<std::byte>(value));
		}

		void appendBigEndianU32(utils::OwnedStdVector<std::byte> &target, uint32_t value) {
			for (uint32_t shift = 24U;; shift -= 8U) {
				appendByte(target, static_cast<uint8_t>((value >> shift) & UINT32_C(0xFF)));
				if (shift == 0U) {
					return;
				}
			}
		}

		void appendBigEndianU64(utils::OwnedStdVector<std::byte> &target, uint64_t value) {
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

		void appendName(utils::OwnedStdVector<std::byte> &target, std::string_view name) {
			for (const char character: name) {
				appendByte(target, static_cast<uint8_t>(static_cast<unsigned char>(character)));
			}
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> bootstrapKey() {
			return {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_BOOTSTRAP)};
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> propertyNameKey(std::string_view name) {
			validateName(name);
			utils::OwnedStdVector<std::byte> key;
			key.reserve(1U + name.size());
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_PROPERTY_NAME));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> propertyTokenPrefix(uint32_t token) {
			utils::OwnedStdVector<std::byte> key;
			key.reserve(1U + sizeof(token));
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_PROPERTY_TOKEN));
			appendBigEndianU32(key, token);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> propertyTokenKey(uint32_t token, std::string_view name) {
			validateName(name);
			utils::OwnedStdVector<std::byte> key = propertyTokenPrefix(token);
			key.reserve(key.size() + name.size());
			appendName(key, name);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> schemaNameKey(SystemCatalogSchemaTokenKind kind, std::string_view name) {
			validateSchemaKind(kind);
			validateName(name);
			utils::OwnedStdVector<std::byte> key;
			key.reserve(2U + name.size());
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_SCHEMA_NAME));
			appendByte(key, static_cast<uint8_t>(kind));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> schemaTokenPrefix(SystemCatalogSchemaTokenKind kind, uint64_t token) {
			validateSchemaKind(kind);
			utils::OwnedStdVector<std::byte> key;
			key.reserve(2U + sizeof(token));
			appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_SCHEMA_TOKEN));
			appendByte(key, static_cast<uint8_t>(kind));
			appendBigEndianU64(key, token);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token,
															std::string_view name) {
			validateName(name);
			utils::OwnedStdVector<std::byte> key = schemaTokenPrefix(kind, token);
			key.reserve(key.size() + name.size());
			appendName(key, name);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> sequenceKey(SystemCatalogSequenceKind kind) {
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

		[[nodiscard]] utils::OwnedStdVector<std::byte> configurationPrefix(std::string_view name) {
			validateName(name);
			utils::OwnedStdVector<std::byte> key = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
			appendBigEndianU32(key, static_cast<uint32_t>(name.size()));
			appendName(key, name);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> configurationKey(std::string_view name, const PropertyValue &value) {
			validateConfigurationValue(value);
			utils::OwnedStdVector<std::byte> key = configurationPrefix(name);
            key.reserve(utils::detail::addStandardHeapBytes(key.size(), CanonicalPropertyValueCodec::encodedSize(value)));
			CanonicalPropertyValueCodec::append(key, value);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> graphCardinalityKey(SystemCatalogGraphCardinalityKind kind) {
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
			const utils::OwnedStdVector<std::byte> prefix = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
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

		[[nodiscard]] utils::OwnedStdVector<std::byte> definitionPrefix(SystemCatalogKeyKind kind) {
			return {static_cast<std::byte>(kind)};
		}

		[[nodiscard]] bool startsWith(std::span<const std::byte> bytes, std::span<const std::byte> prefix);

		[[nodiscard]] utils::OwnedStdVector<std::byte> indexDefinitionLowerBound(uint64_t id) {
			if (!isIndexDefinitionId(id)) { // callers validate IDs before this private encoder.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition ID is outside its namespace");
			}
			utils::OwnedStdVector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
			appendBigEndianU64(key, id);
			return key;
		}

		[[nodiscard]] utils::OwnedStdVector<std::byte> constraintDefinitionLowerBound(uint64_t id) {
			if (!isConstraintDefinitionId(id)) { // callers validate IDs before this private encoder.
				fail(FormatError::FERR_INVALID_HEADER,
					 "System Catalog constraint definition ID is outside its namespace");
			}
			utils::OwnedStdVector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
			appendBigEndianU64(key, id);
			return key;
		}

        [[nodiscard]] utils::OwnedStdVector<std::byte> indexDefinitionKey(const IndexDefinition &definition) {
            const auto encoded = IndexDefinitionCodec::encodeOwned(definition);
            utils::OwnedStdVector<std::byte> key;
            key.reserve(utils::detail::addStandardHeapBytes(1U + sizeof(uint64_t), encoded.size()));
            // The fixed prefix writes borrow a fully admitted, sufficient capacity.
            appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION));
            appendBigEndianU64(key, definition.index_id);
            key.insert(key.end(), encoded.begin(), encoded.end());
            return key;
        }

        [[nodiscard]] utils::OwnedStdVector<std::byte> constraintDefinitionKey(const ConstraintDefinition &definition) {
            const auto encoded = ConstraintDefinitionCodec::encodeOwned(definition);
            utils::OwnedStdVector<std::byte> key;
            key.reserve(utils::detail::addStandardHeapBytes(1U + sizeof(uint64_t), encoded.size()));
            // The fixed prefix writes borrow a fully admitted, sufficient capacity.
            appendByte(key, static_cast<uint8_t>(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION));
            appendBigEndianU64(key, definition.constraint_id);
            key.insert(key.end(), encoded.begin(), encoded.end());
            return key;
        }

		[[nodiscard]] utils::OwnedStdVector<std::byte> namedKey(SystemCatalogKeyKind kind, std::string_view name) {
			validateName(name);
			utils::OwnedStdVector<std::byte> key = definitionPrefix(kind);
            key.reserve(1U + name.size());
			appendName(key, name);
			return key;
		}

		void appendLengthPrefixedString(utils::OwnedStdVector<std::byte> &key, std::string_view value) {
			if (value.empty() || value.size() > std::numeric_limits<uint16_t>::max()) { // codec validates it.
				fail(FormatError::FERR_INVALID_HEADER, "System Catalog semantic key contains an invalid string");
			}
			key.push_back(static_cast<std::byte>((value.size() >> 8U) & UINT16_C(0xFF)));
			key.push_back(static_cast<std::byte>(value.size() & UINT16_C(0xFF)));
			appendName(key, value);
		}

        [[nodiscard]] size_t indexSemanticKeyBytes(const IndexDefinition &definition) {
            size_t bytes = 19U;
            if (definition.kind == IndexDefinitionKind::IDK_VECTOR)
                bytes = utils::detail::addStandardHeapBytes(bytes, definition.vector_metric.size());
            for (const auto &property : definition.property_names)
                bytes = utils::detail::addStandardHeapBytes(bytes, utils::detail::addStandardHeapBytes(2U, property.size()));
            return bytes;
        }


		[[nodiscard]] utils::OwnedStdVector<std::byte> indexSemanticKey(const IndexDefinition &definition) {
			utils::OwnedStdVector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_SEMANTIC);
            key.reserve(indexSemanticKeyBytes(definition));
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

		[[nodiscard]] utils::OwnedStdVector<std::byte> constraintSemanticKey(const ConstraintDefinition &definition) {
			utils::OwnedStdVector<std::byte> key = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_SEMANTIC);
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
			const utils::OwnedStdVector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
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
			const utils::OwnedStdVector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
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

		template<class String = std::string>
        [[nodiscard]] std::optional<String> findReverseName(const SystemCatalog &catalog,
																 const MetadataSnapshotPin &snapshot,
																 std::span<const std::byte> prefix,
																 uint64_t expectedValue, const std::function<void(size_t)> &nameAdmission = {}) {
            static_assert(std::is_same_v<String, std::string> || std::is_same_v<String, utils::OwnedStdString>);
            std::optional<utils::OwnedAllocationScope> allocation;
            if constexpr (std::is_same_v<String, utils::OwnedStdString>) allocation.emplace(schemaAllocationOwner(snapshot));
			std::optional<BTreePageEntry> entry;
			{
				BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore());
				auto cursor = index.openRangeCursor(snapshot, prefix, 2U);
				bool boundaryCharged = false;
				const auto admitBoundary = [&](size_t bytes) {
					if (!nameAdmission) return;
					const size_t retained = entry && !entry->key.hasHeapAdmission() ? entry->key.capacity() : 0U;
					if (bytes > std::numeric_limits<size_t>::max() - retained)
						throw std::overflow_error("System Catalog cursor boundary size overflow");
					const size_t total = retained + bytes;
					// Cursor resets do not allocate. Notify a zero only when it
					// releases a previously accepted charge, not before any copy.
					if (total == 0U && !boundaryCharged) return;
					nameAdmission(total);
					boundaryCharged = total != 0U;
				};
				entry = cursor.next(admitBoundary);
				if (!entry || !startsWith(entry->key, prefix)) return std::nullopt;
				if (nameAdmission && !entry->key.hasHeapAdmission()) nameAdmission(entry->key.capacity());
				auto successor = cursor.next(admitBoundary);
				if (entry->value_or_child != expectedValue || entry->key.size() == prefix.size() ||
					(successor && startsWith(successor->key, prefix))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "System Catalog reverse mapping is malformed, ambiguous, or mismatched");
				}
			}
			// Release cursor pages and the ambiguity probe before allocating the name.
			const size_t nameSize = entry->key.size() - prefix.size();
			validateName(std::string_view(reinterpret_cast<const char *>(entry->key.data() + prefix.size()), nameSize));
			const auto admitName = [&](size_t bytes) {
				if (!nameAdmission) return;
				const size_t keyBytes = entry->key.hasHeapAdmission() ? 0U : entry->key.capacity();
                if (bytes > std::numeric_limits<size_t>::max() - keyBytes)
					throw std::overflow_error("System Catalog reverse name buffer size overflow");
				nameAdmission(keyBytes + bytes);
			};
			if (nameAdmission) admitName(std::max(nameSize, std::string{}.capacity()));
            else if constexpr (std::is_same_v<String, std::string>) detail::admitScopedPropertyString(nameSize);
			String name(std::string_view(reinterpret_cast<const char *>(entry->key.data() + prefix.size()), nameSize));
			admitName(name.capacity());
			entry.reset();
			if (nameAdmission) nameAdmission(name.capacity());
			return name;
		}

	} // namespace

	std::vector<std::byte> SystemCatalogTokenKeyCodec::bootstrapKey() {
        auto key = bootstrapKeyOwned();
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::bootstrapKeyOwned() { return ::graph::storage::bootstrapKey(); }

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyNameKey(std::string_view name) {
        auto key = propertyNameKeyOwned(name);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::propertyNameKeyOwned(std::string_view name) {
		return ::graph::storage::propertyNameKey(name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenKey(uint32_t token, std::string_view name) {
        auto key = propertyTokenKeyOwned(token, name);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenKeyOwned(uint32_t token, std::string_view name) {
		return ::graph::storage::propertyTokenKey(token, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaNameKey(SystemCatalogSchemaTokenKind kind,
																	 std::string_view name) {
        auto key = schemaNameKeyOwned(kind, name);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::schemaNameKeyOwned(SystemCatalogSchemaTokenKind kind,
																	 std::string_view name) {
		return ::graph::storage::schemaNameKey(kind, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenKey(SystemCatalogSchemaTokenKind kind, uint64_t token,
																	  std::string_view name) {
        auto key = schemaTokenKeyOwned(kind, token, name);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenKeyOwned(SystemCatalogSchemaTokenKind kind, uint64_t token,
																	  std::string_view name) {
		return ::graph::storage::schemaTokenKey(kind, token, name);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenSequenceKey() {
        auto key = propertyTokenSequenceKeyOwned();
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::propertyTokenSequenceKeyOwned() {
		return ::graph::storage::sequenceKey(SystemCatalogSequenceKind::SCSK_PROPERTY_TOKEN);
	}

	std::vector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenSequenceKey() {
        auto key = schemaTokenSequenceKeyOwned();
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenKeyCodec::schemaTokenSequenceKeyOwned() {
		return ::graph::storage::sequenceKey(SystemCatalogSequenceKind::SCSK_SCHEMA_TOKEN);
	}

	void SystemCatalogTokenKeyCodec::validateName(std::string_view name) { ::graph::storage::validateName(name); }

	void SystemCatalogTokenKeyCodec::validateSchemaKind(SystemCatalogSchemaTokenKind kind) {
		::graph::storage::validateSchemaKind(kind);
	}

	std::vector<std::byte> SystemCatalogGraphCardinalityCodec::nodeCountKey() {
        auto key = nodeCountKeyOwned();
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned() {
		return ::graph::storage::graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_NODE_COUNT);
	}

	std::vector<std::byte> SystemCatalogGraphCardinalityCodec::edgeCountKey() {
        auto key = edgeCountKeyOwned();
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned() {
		return ::graph::storage::graphCardinalityKey(SystemCatalogGraphCardinalityKind::SCGCK_EDGE_COUNT);
	}

	uint64_t SystemCatalogGraphCardinalityCodec::encodeCount(uint64_t count) {
		return ::graph::storage::encodeGraphCardinalityCount(count);
	}

	uint64_t SystemCatalogGraphCardinalityCodec::decodeCount(uint64_t encoded) {
		return ::graph::storage::decodeGraphCardinalityCount(encoded);
	}

	std::vector<std::byte> SystemCatalogTokenCardinalityCodec::countKey(SystemCatalogSchemaTokenKind kind,
			uint64_t token) {
        auto key = countKeyOwned(kind, token);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenCardinalityCodec::countKeyOwned(SystemCatalogSchemaTokenKind kind,
			uint64_t token) {
		SystemCatalogTokenKeyCodec::validateSchemaKind(kind);
		if (token == 0U && kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL) {
			fail(FormatError::FERR_INVALID_HEADER, "label cardinality requires a nonzero Token");
		}
		utils::OwnedStdVector<std::byte> key{static_cast<std::byte>(SystemCatalogKeyKind::SCKK_GRAPH_CARDINALITY),
			kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL ? std::byte{3U} : std::byte{4U}};
		appendBigEndianU64(key, token);
		return key;
	}

	std::vector<std::byte> SystemCatalogTokenCardinalityCodec::completeKey(SystemCatalogSchemaTokenKind kind) {
        auto key = completeKeyOwned(kind);
        return std::move(static_cast<std::vector<std::byte> &>(key));
    }

    utils::OwnedStdVector<std::byte> SystemCatalogTokenCardinalityCodec::completeKeyOwned(SystemCatalogSchemaTokenKind kind) {
		SystemCatalogTokenKeyCodec::validateSchemaKind(kind);
		return {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_GRAPH_CARDINALITY), kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL ? std::byte{5U} : std::byte{6U}};
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

	SystemCatalogWriteSession::SystemCatalogWriteSession(SystemCatalogWriteSession &&other) noexcept(nothrow_move) :
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

	void SystemCatalogWriteSession::stageUpsert(utils::OwnedStdVector<std::byte> key, uint64_t value) {
        utils::OwnedAllocationScope allocation(allocationOwner());
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

	void SystemCatalogWriteSession::stageErase(const utils::OwnedStdVector<std::byte> &key) {
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		changed_ = true;
		if (initial_root_) {
			if (const auto found = initial_entries_.find(key); found != initial_entries_.end()) initial_entries_.erase(found);
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
        utils::OwnedAllocationScope allocation(allocationOwner());
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
			staged_property_tokens_.emplace(utils::OwnedStdString(name), *existing);
			return *existing;
		}
		const uint64_t allocated = nextPropertyToken();
		const uint32_t token = static_cast<uint32_t>(allocated);
		stageUpsert(propertyNameKey(name), token);
		stageUpsert(propertyTokenKey(token, name), token);
		staged_property_tokens_.emplace(utils::OwnedStdString(name), token);
		return token;
	}

	uint64_t SystemCatalogWriteSession::ensureSchemaToken(SystemCatalogSchemaTokenKind kind, std::string_view name) {
        utils::OwnedAllocationScope allocation(allocationOwner());
		validateSchemaKind(kind);
		validateName(name);
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		const auto staged = staged_schema_tokens_.find(std::pair{kind, name});
		if (staged != staged_schema_tokens_.end()) {
			return staged->second;
		}
		if (const std::optional<uint64_t> existing =
					catalog_->findSchemaToken(transaction_->baseSnapshot(), kind, name);
			existing.has_value()) {
			staged_schema_tokens_.emplace(std::make_pair(kind, utils::OwnedStdString(name)), *existing);
			return *existing;
		}
		const uint64_t token = nextSchemaToken();
		stageUpsert(schemaNameKey(kind, name), token);
		stageUpsert(schemaTokenKey(kind, token, name), token);
		staged_schema_tokens_.emplace(std::make_pair(kind, utils::OwnedStdString(name)), token);
		return token;
	}

	IndexDefinition SystemCatalogWriteSession::createIndexDefinition(IndexDefinition definition) {
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		if (definition.index_id != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog allocates Index definition IDs");
		}
		// Validate and reject duplicate definitions before staging a durable sequence
		// advance. A caller may recover from validation failure and commit other work.
		definition.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
		IndexDefinitionCodec::validate(definition);
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
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		if (definition.constraint_id != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog allocates constraint definition IDs");
		}
		// As with Index definitions, leave the transaction overlay untouched until
		// validation and duplicate checks have succeeded.
		definition.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		ConstraintDefinitionCodec::validate(definition);
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
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		utils::OwnedStdString ownedName(name);
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
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		stageUpsert(SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(),
					SystemCatalogGraphCardinalityCodec::encodeCount(statistics.node_count));
		stageUpsert(SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned(),
					SystemCatalogGraphCardinalityCodec::encodeCount(statistics.edge_count));
		staged_graph_statistics_ = statistics;
	}

	void SystemCatalogWriteSession::setTokenCardinality(SystemCatalogSchemaTokenKind kind, uint64_t token,
			uint64_t count) {
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		stageUpsert(SystemCatalogTokenCardinalityCodec::countKeyOwned(kind, token),
				SystemCatalogGraphCardinalityCodec::encodeCount(count));
	}

	void SystemCatalogWriteSession::markTokenCardinalityComplete(SystemCatalogSchemaTokenKind kind) {
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr || finished_) {
			throw std::logic_error("System Catalog write session is not active");
		}
		stageUpsert(SystemCatalogTokenCardinalityCodec::completeKeyOwned(kind), 1U);
	}

	void SystemCatalogWriteSession::dropIndexDefinition(const IndexDefinition &definition) {
        utils::OwnedAllocationScope allocation(allocationOwner());
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
        utils::OwnedAllocationScope allocation(allocationOwner());
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
        utils::OwnedAllocationScope allocation(allocationOwner());
		if (catalog_ == nullptr || transaction_ == nullptr) {
			throw std::logic_error("System Catalog write session is detached");
		}
		if (finished_) {
			return;
		}
		if (initial_root_ && changed_) {
			utils::OwnedStdVector<BTreePageEntry> entries;
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
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
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
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		const BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
		// Persisted namespaces put bootstrap before the ordered Node/Edge keys.
		// Exact sorted probes reuse authenticated pages without scanning gaps.
		const utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{
			bootstrapKey(), SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(),
			SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned()};
		const auto values = index.findSortedOwned(snapshot, keys);
		if (!values[0] || *values[0] != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		}
		const auto &nodes = values[1];
		const auto &edges = values[2];
		if (nodes.has_value() != edges.has_value()) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog graph cardinality record is incomplete");
		}
		if (!nodes.has_value()) {
			return std::nullopt;
		}
		return NativeGraphCardinalityStatistics{SystemCatalogGraphCardinalityCodec::decodeCount(*nodes),
												SystemCatalogGraphCardinalityCodec::decodeCount(*edges)};
	}

	bool SystemCatalog::hasCompleteTokenCardinality(const MetadataSnapshotPin &snapshot,
			SystemCatalogSchemaTokenKind kind) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const auto key = SystemCatalogTokenCardinalityCodec::completeKeyOwned(kind);
		if (!hasRoot(*this, snapshot)) return false;
		verifyBootstrap(*this, snapshot);
		const auto complete = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
		if (!complete.has_value()) return false;
		if (*complete != 1U) fail(FormatError::FERR_INVALID_STATE, "unsupported token cardinality completeness record");
		return true;
	}

	std::optional<uint64_t> SystemCatalog::readTokenCardinality(const MetadataSnapshotPin &snapshot,
			SystemCatalogSchemaTokenKind kind, uint64_t token) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const auto key = SystemCatalogTokenCardinalityCodec::countKeyOwned(kind, token);
		const auto completeKey = SystemCatalogTokenCardinalityCodec::completeKeyOwned(kind);
		if (!hasRoot(*this, snapshot)) return std::nullopt;
		// The count namespace precedes its completeness marker in the persisted
		// codec. Read all three exact keys through one authenticated tree probe.
		const utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{bootstrapKey(), key, completeKey};
		const auto values = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).findSortedOwned(snapshot, keys);
		if (!values[0] || *values[0] != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		if (!values[2]) return std::nullopt;
		if (*values[2] != 1U)
			fail(FormatError::FERR_INVALID_STATE, "unsupported token cardinality completeness record");
		return values[1] ? SystemCatalogGraphCardinalityCodec::decodeCount(*values[1]) : 0U;
	}

	std::vector<std::optional<uint64_t>> SystemCatalog::readTokenCardinalities(
		const MetadataSnapshotPin &snapshot, SystemCatalogSchemaTokenKind kind,
		std::span<const uint64_t> tokens) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const auto completeKey = SystemCatalogTokenCardinalityCodec::completeKeyOwned(kind);
		utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{bootstrapKey()};
		for (size_t i = 0U; i < tokens.size(); ++i) {
			if (i && tokens[i - 1U] >= tokens[i])
				fail(FormatError::FERR_INVALID_STATE, "token count batch requires strictly increasing tokens");
			keys.push_back(SystemCatalogTokenCardinalityCodec::countKeyOwned(kind, tokens[i]));
		}
		keys.push_back(completeKey);
		utils::OwnedStdVector<std::optional<uint64_t>> result(tokens.size());
		if (tokens.empty() || !hasRoot(*this, snapshot)) return result;
		const auto values = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).findSortedOwned(snapshot, keys);
		if (!values.front() || *values.front() != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		if (!values.back()) return result;
		if (*values.back() != 1U)
			fail(FormatError::FERR_INVALID_STATE, "unsupported token cardinality completeness record");
		for (size_t i = 0U; i < tokens.size(); ++i)
			result[i] = values[i + 1U] ? SystemCatalogGraphCardinalityCodec::decodeCount(*values[i + 1U]) : 0U;
		return result;
	}

	std::optional<std::pair<NativeGraphCardinalityStatistics, uint64_t>>
	SystemCatalog::readTokenCardinalityEstimate(const MetadataSnapshotPin &snapshot,
			SystemCatalogSchemaTokenKind kind, uint64_t token) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const auto countKey = SystemCatalogTokenCardinalityCodec::countKeyOwned(kind, token);
		const auto completeKey = SystemCatalogTokenCardinalityCodec::completeKeyOwned(kind);
		if (!hasRoot(*this, snapshot)) return std::nullopt;
		// These five persisted keys are ordered by namespace. Reuse one
		// authenticated exact probe rather than decoding the same leaf for
		// each statistic; completeness still gates interpretation of counts.
		const utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{
			bootstrapKey(), SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(),
			SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned(), countKey, completeKey};
		const auto values = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).findSortedOwned(snapshot, keys);
		if (!values[0] || *values[0] != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		if (!values[4]) return std::nullopt;
		if (*values[4] != 1U)
			fail(FormatError::FERR_INVALID_STATE, "unsupported token cardinality completeness record");
		if (values[1].has_value() != values[2].has_value())
			fail(FormatError::FERR_INVALID_STATE, "System Catalog graph cardinality record is incomplete");
		if (!values[1]) return std::nullopt;
		return std::pair{NativeGraphCardinalityStatistics{
				SystemCatalogGraphCardinalityCodec::decodeCount(*values[1]),
				SystemCatalogGraphCardinalityCodec::decodeCount(*values[2])},
				values[3] ? SystemCatalogGraphCardinalityCodec::decodeCount(*values[3]) : 0U};
	}

	utils::OwnedVector<std::optional<utils::OwnedStdString>> SystemCatalog::findPropertyNamesSorted(
		const MetadataSnapshotPin &snapshot, std::span<const uint32_t> tokens) const {
		utils::OwnedAllocationScope allocation(schemaAllocationOwner(snapshot));
		for (size_t i = 0U; i < tokens.size(); ++i) {
			if (tokens[i] == 0U || (i != 0U && tokens[i - 1U] >= tokens[i]))
				fail(FormatError::FERR_INVALID_HEADER, "Property name batch requires strictly increasing nonzero tokens");
		}
		if (tokens.size() == std::numeric_limits<size_t>::max())
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property name cursor bound overflowed");
		utils::OwnedVector<std::optional<utils::OwnedStdString>> result(tokens.size());
		if (tokens.empty() || !hasRoot(*this, snapshot)) return result;
		verifyBootstrap(*this, snapshot);
		BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
		for (size_t first = 0U; first < tokens.size();) {
			size_t end = first + 1U;
			while (end < tokens.size() && uint64_t(tokens[end]) == uint64_t(tokens[end - 1U]) + 1U) ++end;
			const auto lower = propertyTokenPrefix(tokens[first]);
			// One successor proves uniqueness of the last mapping. Starting a new
			// cursor at each nonconsecutive token avoids traversing unrelated gaps.
			auto cursor = index.openRangeCursor(snapshot, lower, end - first + 1U);
			auto entry = cursor.next();
			for (size_t i = first; i < end; ++i) {
				const auto prefix = propertyTokenPrefix(tokens[i]);
				if (!entry || !startsWith(entry->key, prefix)) continue;
				if (entry->value_or_child != tokens[i] || entry->key.size() == prefix.size())
					fail(FormatError::FERR_INVALID_STATE, "System Catalog reverse mapping is malformed or mismatched");
				auto successor = cursor.next();
				if (successor && startsWith(successor->key, prefix))
					fail(FormatError::FERR_INVALID_STATE, "System Catalog reverse mapping is ambiguous");
				const std::string_view name(reinterpret_cast<const char *>(entry->key.data() + prefix.size()),
					entry->key.size() - prefix.size());
				validateName(name);
				result[i].emplace(name);
				entry = std::move(successor);
			}
			first = end;
		}
		utils::OwnedStdVector<size_t> order;
		order.reserve(tokens.size());
		for (size_t i = 0U; i < result.size(); ++i) if (result[i]) order.push_back(i);
		const auto nameAt = [&](size_t i) { return std::string_view(result[i]->data(), result[i]->size()); };
		std::ranges::sort(order, [&](size_t left, size_t right) { return nameAt(left) < nameAt(right); });
		utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys;
		keys.reserve(order.size());
		for (size_t i = 0U; i < order.size(); ++i) {
			if (i != 0U && nameAt(order[i - 1U]) == nameAt(order[i]))
				fail(FormatError::FERR_INVALID_STATE, "System Catalog reverse mappings contain duplicate names");
			keys.push_back(propertyNameKey(nameAt(order[i])));
		}
		if (!keys.empty()) {
			const auto values = index.findSortedOwned(snapshot, keys);
			for (size_t i = 0U; i < order.size(); ++i)
				if (!values[i] || *values[i] != tokens[order[i]])
					fail(FormatError::FERR_INVALID_STATE, "System Catalog Property forward and reverse mappings disagree");
		}
		return result;
	}

	std::optional<std::string> SystemCatalog::findPropertyName(const MetadataSnapshotPin &snapshot,
															   uint32_t token, const std::function<void(size_t)> &nameAdmission) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (token == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog Property-key lookup requires a nonzero Token");
		}
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		if (!nameAdmission && !catalogOwnedHeapAdmission()) detail::admitScopedPropertyVectorStorage<std::byte>(1U + sizeof(token));
        auto name = findReverseName(*this, snapshot, propertyTokenPrefix(token), token, nameAdmission);
		if (!name.has_value()) {
			return std::nullopt;
		}
		std::optional<uint64_t> forward;
		{
			if (name->capacity() > std::numeric_limits<size_t>::max() - 1U ||
				name->size() > std::numeric_limits<size_t>::max() - name->capacity() - 1U) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property name admission size overflowed");
			}
			if (nameAdmission) nameAdmission(name->capacity() + (catalogOwnedHeapAdmission() ? 0U : name->size() + 1U));
            else if (!catalogOwnedHeapAdmission()) detail::admitScopedPropertyVectorStorage<std::byte>(1U + name->size());
			const auto key = propertyNameKey(*name);
			if (nameAdmission) nameAdmission(name->capacity() + (key.hasHeapAdmission() ? 0U : key.capacity()));
			forward = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
		}
		if (nameAdmission) nameAdmission(name->capacity());
		if (!forward.has_value() || *forward != token) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog Property-key reverse mapping has no matching forward mapping");
		}
		return name;
	}

	std::optional<uint64_t> SystemCatalog::findSchemaToken(const MetadataSnapshotPin &snapshot,
														   SystemCatalogSchemaTokenKind kind,
														   std::string_view name) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		validateSchemaKind(kind);
		validateName(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		const utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{bootstrapKey(), schemaNameKey(kind, name)};
		const auto values = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).findSortedOwned(snapshot, keys);
		if (!values[0] || *values[0] != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		const auto &value = values[1];
		if (!value.has_value()) {
			return std::nullopt;
		}
		if (*value == 0U) {
			fail(FormatError::FERR_INVALID_STATE, "System Catalog schema mapping has an invalid Token");
		}
		// The forward mapping is already authenticated in this pinned snapshot.
		// Validate reverse uniqueness/value/name directly, without calling the
		// public reverse lookup that rereads bootstrap and this same forward key.
		const auto reverse = findReverseName(*this, snapshot, schemaTokenPrefix(kind, *value), *value);
		if (!reverse.has_value() || *reverse != name) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog schema forward mapping has no matching reverse mapping");
		}
		return *value;
	}

	std::vector<std::optional<uint64_t>> SystemCatalog::findSchemaTokens(
		const MetadataSnapshotPin &snapshot, SystemCatalogSchemaTokenKind kind,
		std::span<const std::string> names) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		validateSchemaKind(kind);
		utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys{bootstrapKey()};
		for (const auto &name : names) {
			auto key = schemaNameKey(kind, name);
			if (!std::lexicographical_compare(keys.back().begin(), keys.back().end(), key.begin(), key.end()))
				fail(FormatError::FERR_INVALID_STATE, "schema name batch requires strictly increasing keys");
			keys.push_back(std::move(key));
		}
		utils::OwnedStdVector<std::optional<uint64_t>> result(names.size());
		if (names.empty() || !hasRoot(*this, snapshot)) return result;
		const auto values = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).findSortedOwned(snapshot, keys);
		if (!values.front() || *values.front() != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
			fail(FormatError::FERR_INVALID_STATE, "System Catalog root is missing its bootstrap record");
		for (size_t i = 0U; i < names.size(); ++i) {
			const auto value = values[i + 1U];
			if (!value) continue;
			if (*value == 0U)
				fail(FormatError::FERR_INVALID_STATE, "System Catalog schema mapping has an invalid Token");
			const auto reverse = findReverseName(*this, snapshot, schemaTokenPrefix(kind, *value), *value);
			if (!reverse || *reverse != names[i])
				fail(FormatError::FERR_INVALID_STATE, "System Catalog schema forward mapping has no matching reverse mapping");
			result[i] = value;
		}
		return result;
	}

	std::optional<SystemCatalogSchemaToken> SystemCatalog::findSchemaToken(const MetadataSnapshotPin &snapshot,
																		   SystemCatalogSchemaTokenKind kind,
																		   uint64_t token, const std::function<void(size_t)> &nameAdmission) const {
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		validateSchemaKind(kind);
		if (token == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog schema lookup requires a nonzero Token");
		}
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		if (!nameAdmission && !catalogOwnedHeapAdmission()) detail::admitScopedPropertyVectorStorage<std::byte>(2U + sizeof(token));
        auto name = findReverseName(*this, snapshot, schemaTokenPrefix(kind, token), token, nameAdmission);
		if (!name.has_value()) {
			return std::nullopt;
		}
		std::optional<uint64_t> forward;
		{
			if (name->capacity() > std::numeric_limits<size_t>::max() - 2U ||
				name->size() > std::numeric_limits<size_t>::max() - name->capacity() - 2U) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "schema name admission size overflowed");
			}
			if (nameAdmission) nameAdmission(name->capacity() + (catalogOwnedHeapAdmission() ? 0U : name->size() + 2U));
            else if (!catalogOwnedHeapAdmission()) detail::admitScopedPropertyVectorStorage<std::byte>(2U + name->size());
			const auto key = schemaNameKey(kind, *name);
			if (nameAdmission) nameAdmission(name->capacity() + (key.hasHeapAdmission() ? 0U : key.capacity()));
			forward = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
		}
		if (nameAdmission) nameAdmission(name->capacity());
		if (!forward.has_value() || *forward != token) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog schema reverse mapping has no matching forward mapping");
		}
		return SystemCatalogSchemaToken{token, kind, std::move(*name)};
	}

    std::optional<OwnedSystemCatalogSchemaToken> SystemCatalog::findOwnedSchemaToken(
            const MetadataSnapshotPin &snapshot, SystemCatalogSchemaTokenKind kind, uint64_t token) const {
        utils::OwnedAllocationScope allocation(schemaAllocationOwner(snapshot));
        validateSchemaKind(kind);
        if (token == 0U) fail(FormatError::FERR_INVALID_HEADER, "System Catalog schema lookup requires a nonzero Token");
        if (!hasRoot(*this, snapshot)) return std::nullopt;
        verifyBootstrap(*this, snapshot);
        const auto prefix = schemaTokenPrefix(kind, token);
        auto name = findReverseName<utils::OwnedStdString>(*this, snapshot, prefix, token);
        if (!name) return std::nullopt;
        const auto key = schemaNameKey(kind, *name);
        const auto forward = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).find(snapshot, key);
        if (!forward || *forward != token)
            fail(FormatError::FERR_INVALID_STATE, "System Catalog schema reverse mapping has no matching forward mapping");
        return OwnedSystemCatalogSchemaToken{token, kind, std::move(*name)};
    }

	std::optional<IndexDefinition> SystemCatalog::findIndexDefinitionById(const MetadataSnapshotPin &snapshot,
																		  uint64_t indexId) const {
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		if (indexId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog Index definition lookup requires a nonzero ID");
		}
		if (!isIndexDefinitionId(indexId) || !hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<std::byte> lower = indexDefinitionLowerBound(indexId);
		const utils::OwnedStdVector<BTreePageEntry> entries =
				BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_).scan(snapshot, lower, 2U);
		if (entries.empty() || !startsWith(entries.front().key, lower)) {
			return std::nullopt;
		}
		if (entries.size() == 2U && startsWith(entries[1].key, lower)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "System Catalog contains multiple Index definitions for one logical ID");
		}
		IndexDefinition definition = decodeIndexDefinition(entries.front().key);
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
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
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
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		IndexDefinition probe;
		probe.name = "_";
		probe.entity_kind = entityKind;
		probe.kind = kind;
		probe.label_token_id = labelTokenId;
		probe.property_names.reserve(propertyNames.size());
        for (const auto &name : propertyNames) probe.property_names.emplace_back(name);
		probe.vector_dimensions = vectorDimensions;
		probe.vector_metric = vectorMetric;
		probe.index_id = RSS_SYSTEM_CATALOG_INDEX_ID_FIRST;
		IndexDefinitionCodec::validate(probe);
		const utils::OwnedStdVector<std::byte> key = indexSemanticKey(probe);
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

    SystemCatalog::FieldMutationSchema SystemCatalog::readFieldMutationSchema(const MetadataSnapshotPin &snapshot,
        size_t maximumRecords, const CancellationCheckpoint *cancellation) const {
        if (!metadata_.ownsSnapshot(snapshot) || !maximumRecords || maximumRecords > (SIZE_MAX - 2U) / 2U)
            fail(FormatError::FERR_INVALID_HEADER, "native field schema proof requires its pin and finite control bound");
        utils::OwnedAllocationScope allocation(schemaAllocationOwner(snapshot));
        const auto check = [&] { if (cancellation) cancellation->check(); };
        check();
        auto definitions = readIndexDefinitions(snapshot, maximumRecords);
        for (const auto &definition : definitions) {
            check(); if (definition.kind != IndexDefinitionKind::IDK_LABEL &&
                definition.kind != IndexDefinitionKind::IDK_PROPERTY && definition.kind != IndexDefinitionKind::IDK_COMPOSITE &&
                definition.kind != IndexDefinitionKind::IDK_VECTOR)
                fail(FormatError::FERR_UNSUPPORTED_VERSION, "native field publication requires complete property-derived index coordination");
        }
        verifyBootstrap(*this, snapshot);
        utils::OwnedHeapReservation frontier(utils::OwnedAllocationScope::owner(), 16384U);
        utils::OwnedHeapReservation boundary(utils::OwnedAllocationScope::owner());
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        const BTreeIndex tree(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
        // Prove dependency/control ranges with authenticated seeks. Unchanged
        // token dictionaries, configurations and token counts are independent
        // of field values and must not become a database-size write limit.
        const auto scan = [&](uint8_t first, uint8_t last, size_t bound, const auto &visit) {
            const std::array<std::byte, 1> lower{std::byte(first)};
            const auto begin = first == 0U ? std::optional<std::span<const std::byte>>{} :
                std::optional<std::span<const std::byte>>(std::span<const std::byte>(lower));
            auto cursor = tree.openRangeCursor(snapshot, begin, bound + 1U,
                BufferPoolReadHint::BPRH_SEQUENTIAL, false, cancellation);
            size_t count = 0;
            while (auto record = cursor.next(admit)) {
                check();
                if (record->key.empty()) fail(FormatError::FERR_INVALID_HEADER, "native field schema proof found an empty key");
                const auto kind = std::to_integer<uint8_t>(record->key.front());
                if (kind > last) break;
                if (++count > bound) fail(FormatError::FERR_INVALID_STATE, "native field schema control range exceeds its operation bound");
                visit(*record);
            }
        };
        scan(0, 0, 1, [&](const BTreePageEntry &record) {
            if (record.key != bootstrapKey() || record.value_or_child != RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE)
                fail(FormatError::FERR_INVALID_HEADER, "native field schema proof found an unknown bootstrap record");
        });
        scan(5, 5, 4, [&](const BTreePageEntry &record) {
            if (record.key.size() != 2U || std::to_integer<uint8_t>(record.key[1]) < 1U || std::to_integer<uint8_t>(record.key[1]) > 4U)
                fail(FormatError::FERR_UNSUPPORTED_VERSION, "native field schema proof found an unknown sequence namespace");
        });
        scan(7, 8, maximumRecords * 2U, [&](const BTreePageEntry &record) {
            const auto found = std::ranges::lower_bound(definitions, record.value_or_child, {}, &IndexDefinition::index_id);
            if (found == definitions.end() || found->index_id != record.value_or_child)
                fail(FormatError::FERR_INVALID_STATE, "native field schema proof found an orphan index direction");
            const auto canonical = record.key.front() == std::byte{7U} ?
                namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, found->name) : indexSemanticKey(*found);
            if (!std::ranges::equal(record.key, canonical))
                fail(FormatError::FERR_INVALID_STATE, "native field schema proof found a noncanonical index direction");
        });
        auto constraints = readConstraintDefinitions(snapshot, maximumRecords);
        scan(10, 11, maximumRecords * 2U, [&](const BTreePageEntry &record) {
            const auto found = std::ranges::find(constraints, record.value_or_child, &ConstraintDefinition::constraint_id);
            if (found == constraints.end())
                fail(FormatError::FERR_INVALID_STATE, "native field schema proof found an orphan constraint direction");
            const auto canonical = record.key.front() == std::byte{10U} ?
                namedKey(SystemCatalogKeyKind::SCKK_CONSTRAINT_NAME, found->name) : constraintSemanticKey(*found);
            if (!std::ranges::equal(record.key, canonical))
                fail(FormatError::FERR_INVALID_STATE, "native field schema proof found a noncanonical constraint direction");
        });
        {
            // Current graph/token count subtypes1..6 are field-independent.
            // A future count subtype may depend on properties: refuse it.
            const std::array<std::byte, 2> lower{std::byte{13U}, std::byte{7U}};
            auto cursor = tree.openRangeCursor(snapshot, std::span<const std::byte>(lower), 1,
                BufferPoolReadHint::BPRH_SEQUENTIAL, false, cancellation);
            if (auto record = cursor.next(admit); record && !record->key.empty() && record->key.front() == std::byte{13U})
                fail(FormatError::FERR_UNSUPPORTED_VERSION, "native field schema proof found an unknown count namespace");
        }
        scan(14, 255, 1, [&](const BTreePageEntry &) {
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "native field schema proof found an unknown control namespace");
        });
        check(); return {std::move(definitions), std::move(constraints)};
    }

	utils::OwnedStdVector<IndexDefinition> SystemCatalog::readIndexDefinitions(const MetadataSnapshotPin &snapshot,
																	 size_t maximumDefinitions) const {
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		if (maximumDefinitions == 0U || maximumDefinitions == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog Index definition read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_INDEX_DEFINITION);
		const BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
        // Keep only the cursor's leaf/route and one encoded definition; the
        // validated typed output owns its own backing and strings.
        utils::OwnedHeapReservation boundaryCredit(utils::OwnedAllocationScope::owner());
        auto cursor = index.openRangeCursor(snapshot, prefix, maximumDefinitions + 1U);
        utils::OwnedStdVector<IndexDefinition> result;
        uint64_t previousId = 0U;
        while (auto encoded = cursor.next([&](size_t capacity) {
            boundaryCredit.set(capacity == 0U ? 0U : utils::detail::addStandardHeapBytes(
                utils::detail::standardContainerProxyBytes(),
                utils::detail::standardAllocatorRequestBytes(capacity, alignof(std::byte))));
        })) {
            const BTreePageEntry &entry = *encoded;
			if (!startsWith(entry.key, prefix)) {
				break;
			}
			if (result.size() == maximumDefinitions) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definitions exceed the caller bound");
			}
			const IndexDefinition definition = decodeIndexDefinition(entry.key);
			if (entry.value_or_child != definition.index_id ||
				definition.index_id <= previousId) {
				fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition is not self-consistent");
			}
			previousId = definition.index_id;
			result.push_back(definition);
		}
        // Validate every direction in bounded batches. Duplicate directions in
        // different batches still fail: one canonical key cannot map to two
        // distinct strictly increasing definition IDs.
        struct Direction {
            using OwnedHeapElement = void;
            utils::OwnedStdVector<std::byte> first;
            uint64_t second;
        };
        constexpr size_t definitionsPerBatch = 64U;
        for (size_t first = 0U; first < result.size();) {
            const auto count = (std::min)(definitionsPerBatch, result.size() - first);
            const auto directionsCount = 2U*count;
            utils::OwnedStdVector<Direction> directions;
            directions.reserve(directionsCount);
            for (size_t offset = 0U; offset < count; ++offset) {
                const auto &definition = result[first + offset];
                directions.emplace_back(namedKey(SystemCatalogKeyKind::SCKK_INDEX_NAME, definition.name), definition.index_id);
                directions.emplace_back(indexSemanticKey(definition), definition.index_id);
            }
            std::ranges::sort(directions, [](const auto &left, const auto &right) {
                return std::lexicographical_compare(left.first.begin(), left.first.end(), right.first.begin(), right.first.end());
            });
            utils::OwnedStdVector<utils::OwnedStdVector<std::byte>> keys;
            keys.reserve(directions.size());
            for (auto &direction : directions) {
                if (!keys.empty() && keys.back() == direction.first)
                    fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition directions are duplicated");
                keys.push_back(std::move(direction.first));
            }
            const auto values = index.findSortedOwned(snapshot, keys);
            for (size_t position = 0U; position < values.size(); ++position)
                if (!values[position] || *values[position] != directions[position].second)
                    fail(FormatError::FERR_INVALID_STATE, "System Catalog Index definition directions do not agree");
            first += count;
        }
		return result;
	}

	std::optional<ConstraintDefinition> SystemCatalog::findConstraintDefinitionById(const MetadataSnapshotPin &snapshot,
																					uint64_t constraintId) const {
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		if (constraintId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "System Catalog constraint definition lookup requires a nonzero ID");
		}
		if (!isConstraintDefinitionId(constraintId) || !hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<std::byte> lower = constraintDefinitionLowerBound(constraintId);
		const utils::OwnedStdVector<BTreePageEntry> entries =
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
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
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
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		ConstraintDefinition probe;
		probe.name = "_";
		probe.entity_kind = entityKind;
		probe.kind = kind;
		probe.schema_token_id = schemaTokenId;
		probe.property_names.reserve(propertyNames.size());
        for (const auto &name : propertyNames) probe.property_names.emplace_back(name);
		probe.property_type = propertyType;
		probe.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
		ConstraintDefinitionCodec::validate(probe);
		const utils::OwnedStdVector<std::byte> key = constraintSemanticKey(probe);
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

	utils::OwnedStdVector<ConstraintDefinition> SystemCatalog::readConstraintDefinitions(const MetadataSnapshotPin &snapshot,
																			   size_t maximumDefinitions) const {
        utils::OwnedAllocationScope schemaAllocation(schemaAllocationOwner(snapshot));
		if (maximumDefinitions == 0U || maximumDefinitions == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog constraint definition read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<std::byte> prefix = definitionPrefix(SystemCatalogKeyKind::SCKK_CONSTRAINT_DEFINITION);
        const BTreeIndex index(RSS_SYSTEM_CATALOG_INDEX_ID, pages_, metadata_);
        utils::OwnedHeapReservation boundaryCredit(utils::OwnedAllocationScope::owner());
        auto cursor = index.openRangeCursor(snapshot, prefix, maximumDefinitions + 1U);
        utils::OwnedStdVector<ConstraintDefinition> result;
        while (const auto encoded = cursor.next([&](size_t capacity) {
            boundaryCredit.set(capacity == 0U ? 0U : utils::detail::addStandardHeapBytes(
                utils::detail::standardContainerProxyBytes(),
                utils::detail::standardAllocatorRequestBytes(capacity, alignof(std::byte))));
        })) {
            const BTreePageEntry &entry = *encoded;
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
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const utils::OwnedStdVector<std::byte> prefix = configurationPrefix(name);
		if (!hasRoot(*this, snapshot)) {
			return std::nullopt;
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<BTreePageEntry> entries =
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
        utils::OwnedAllocationScope catalogKeyAllocation(utils::OwnedAllocationScope::active()
            ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "System Catalog configuration read requires a finite positive bound");
		}
		if (!hasRoot(*this, snapshot)) {
			return {};
		}
		verifyBootstrap(*this, snapshot);
		const utils::OwnedStdVector<std::byte> prefix = {static_cast<std::byte>(SystemCatalogKeyKind::SCKK_CONFIGURATION)};
		const utils::OwnedStdVector<BTreePageEntry> entries =
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
