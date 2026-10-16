/**
 * @file IndexDefinition.cpp
 * @brief Explicit, bounded codec for relocatable logical index definitions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace graph::storage {

	namespace {

		constexpr size_t INDEX_ID_OFFSET = 8;
		constexpr size_t LABEL_TOKEN_ID_OFFSET = 16;
		constexpr size_t ENTITY_KIND_OFFSET = 24;
		constexpr size_t INDEX_KIND_OFFSET = 25;
		constexpr size_t PROPERTY_COUNT_OFFSET = 26;
		constexpr size_t NAME_BYTES_OFFSET = 28;
		constexpr size_t METRIC_BYTES_OFFSET = 32;
		constexpr size_t VECTOR_DIMENSIONS_OFFSET = 36;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 40;
		constexpr size_t RESERVED_OFFSET = 44;
		constexpr uint64_t INTERNAL_INDEX_ID_MASK = UINT64_C(1) << 63U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isPublicIndexId(uint64_t indexId) {
			return indexId != 0U && (indexId & INTERNAL_INDEX_ID_MASK) == 0U;
		}

		[[nodiscard]] bool isKnownEntityKind(IndexDefinitionEntityKind kind) {
			return kind == IndexDefinitionEntityKind::IDEK_NODE || kind == IndexDefinitionEntityKind::IDEK_EDGE;
		}

		[[nodiscard]] bool isKnownIndexKind(IndexDefinitionKind kind) {
			return kind == IndexDefinitionKind::IDK_LABEL || kind == IndexDefinitionKind::IDK_PROPERTY ||
				   kind == IndexDefinitionKind::IDK_COMPOSITE || kind == IndexDefinitionKind::IDK_VECTOR;
		}

		[[nodiscard]] bool isSupportedVectorMetric(std::string_view metric) {
			return metric == "l2" || metric == "cosine" || metric == "ip";
		}

		void validateByteString(std::string_view value, uint32_t maximum, const char *message) {
			if (value.empty() || value.size() > maximum) {
				fail(FormatError::FERR_INVALID_HEADER, message);
			}
		}

		template<class Definition> void validateDefinition(const Definition &definition) {
			if (!isPublicIndexId(definition.index_id) || !isKnownEntityKind(definition.entity_kind) ||
				!isKnownIndexKind(definition.kind)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable index definition has an invalid logical identity or enum value");
			}
			validateByteString(definition.name, RSS_INDEX_DEFINITION_MAX_NAME_BYTES,
							   "relocatable index definition name is empty or exceeds its byte bound");
			if (definition.property_names.size() > RSS_INDEX_DEFINITION_MAX_PROPERTIES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable index definition exceeds the supported property-key component count");
			}
			std::array<std::string_view, RSS_INDEX_DEFINITION_MAX_PROPERTIES> unique_names{};
            size_t unique_count = 0U;
			for (const auto &property: definition.property_names) {
				validateByteString(property, RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES,
								   "relocatable index definition property name is empty or exceeds its byte bound");
				if (std::find(unique_names.begin(), unique_names.begin() + unique_count, std::string_view(property)) != unique_names.begin() + unique_count) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable index definition repeats a property-key component");
				}
                unique_names[unique_count++] = property;
			}

			const uint8_t invalidKindContract =
					static_cast<uint8_t>(definition.kind == IndexDefinitionKind::IDK_LABEL &&
										 (definition.label_token_id != 0U || !definition.property_names.empty())) |
					static_cast<uint8_t>(definition.kind == IndexDefinitionKind::IDK_PROPERTY &&
										 definition.property_names.size() != 1U) |
					static_cast<uint8_t>(definition.kind == IndexDefinitionKind::IDK_COMPOSITE &&
										 (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
										  definition.property_names.size() < 2U)) |
					static_cast<uint8_t>(definition.kind == IndexDefinitionKind::IDK_VECTOR &&
										 (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
										  definition.label_token_id == 0U || definition.property_names.size() != 1U ||
										  definition.vector_dimensions == 0U ||
										  !isSupportedVectorMetric(definition.vector_metric))) |
					static_cast<uint8_t>(definition.kind != IndexDefinitionKind::IDK_VECTOR &&
										 (definition.vector_dimensions != 0U || !definition.vector_metric.empty())) |
					static_cast<uint8_t>(definition.entity_kind == IndexDefinitionEntityKind::IDEK_EDGE &&
										 definition.kind == IndexDefinitionKind::IDK_PROPERTY &&
										 definition.label_token_id != 0U);
			if (invalidKindContract != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable index definition has an invalid kind-specific schema contract");
			}
		}

		void appendU16(utils::OwnedStdVector<std::byte> &bytes, uint16_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU16(bytes, offset, value);
		}

		void appendBytes(utils::OwnedStdVector<std::byte> &bytes, std::string_view value) {
		if (!value.empty()) {
			const std::byte *begin = reinterpret_cast<const std::byte *>(value.data());
			bytes.insert(bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(value.size()));
		}
		}

		[[nodiscard]] std::string_view readString(std::span<const std::byte> payload, size_t &offset, size_t length,
											const char *message) {
			if (length > payload.size() - offset) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, message);
			}
			const char *data = reinterpret_cast<const char *>(payload.data() + offset);
			offset += length;
			return {data, length};
		}

	} // namespace

	std::vector<std::byte> IndexDefinitionCodec::encode(const IndexDefinition &definition) {
        auto bytes = encodeOwned(definition);
        // The legacy raw output is caller-owned; native private paths use encodeOwned.
        return std::move(static_cast<std::vector<std::byte> &>(bytes));
    }

    void IndexDefinitionCodec::validate(const IndexDefinition &definition) { validateDefinition(definition); }

    utils::OwnedStdVector<std::byte> IndexDefinitionCodec::encodeOwned(const IndexDefinition &definition) {
		validateDefinition(definition);
        size_t payloadBytes = definition.name.size() + definition.vector_metric.size();
        for (const auto &property : definition.property_names)
            payloadBytes = utils::detail::addStandardHeapBytes(payloadBytes, sizeof(uint16_t) + property.size());
		// ZYX_COV_EXCL_START: the fixed field and component bounds keep this below the 32-bit wire limit.
		if (payloadBytes > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable index definition payload exceeds its wire capacity");
		}
		// ZYX_COV_EXCL_STOP
        utils::OwnedStdVector<std::byte> bytes;
        bytes.reserve(utils::detail::addStandardHeapBytes(RSS_INDEX_DEFINITION_HEADER_BYTES, payloadBytes));
        bytes.resize(RSS_INDEX_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, 0, RSS_INDEX_DEFINITION_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4, RSS_INDEX_DEFINITION_VERSION);
		LittleEndianCodec::writeU16(bytes, 6, RSS_INDEX_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes, INDEX_ID_OFFSET, definition.index_id);
		LittleEndianCodec::writeU64(bytes, LABEL_TOKEN_ID_OFFSET, definition.label_token_id);
		bytes[ENTITY_KIND_OFFSET] = static_cast<std::byte>(definition.entity_kind);
		bytes[INDEX_KIND_OFFSET] = static_cast<std::byte>(definition.kind);
		LittleEndianCodec::writeU16(bytes, PROPERTY_COUNT_OFFSET,
								 static_cast<uint16_t>(definition.property_names.size()));
		LittleEndianCodec::writeU32(bytes, NAME_BYTES_OFFSET, static_cast<uint32_t>(definition.name.size()));
		LittleEndianCodec::writeU32(bytes, METRIC_BYTES_OFFSET, static_cast<uint32_t>(definition.vector_metric.size()));
		LittleEndianCodec::writeU32(bytes, VECTOR_DIMENSIONS_OFFSET, definition.vector_dimensions);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payloadBytes));
		LittleEndianCodec::writeU32(bytes, RESERVED_OFFSET, 0U);
        appendBytes(bytes, definition.name);
        appendBytes(bytes, definition.vector_metric);
        for (const auto &property : definition.property_names) {
            appendU16(bytes, static_cast<uint16_t>(property.size()));
            appendBytes(bytes, property);
        }
		return bytes;
	}

	IndexDefinition IndexDefinitionCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_INDEX_DEFINITION_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable index definition is truncated");
		}
		if (LittleEndianCodec::readU32(bytes, 0) != RSS_INDEX_DEFINITION_MAGIC) {
			fail(FormatError::FERR_INVALID_MAGIC, "relocatable index definition magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, 4) != RSS_INDEX_DEFINITION_VERSION) {
			fail(FormatError::FERR_UNSUPPORTED_VERSION, "relocatable index definition version is unsupported");
		}
		if (LittleEndianCodec::readU16(bytes, 6) != RSS_INDEX_DEFINITION_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, RESERVED_OFFSET) != 0U) {
			fail(FormatError::FERR_RESERVED_BITS, "relocatable index definition fixed or reserved fields are invalid");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_INDEX_DEFINITION_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable index definition payload length does not match its bytes");
		}
		const uint32_t nameBytes = LittleEndianCodec::readU32(bytes, NAME_BYTES_OFFSET);
		const uint32_t metricBytes = LittleEndianCodec::readU32(bytes, METRIC_BYTES_OFFSET);
		const uint16_t propertyCount = LittleEndianCodec::readU16(bytes, PROPERTY_COUNT_OFFSET);
		if (nameBytes > RSS_INDEX_DEFINITION_MAX_NAME_BYTES ||
			metricBytes > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES ||
			propertyCount > RSS_INDEX_DEFINITION_MAX_PROPERTIES ||
			nameBytes > payloadBytes || metricBytes > payloadBytes - nameBytes) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable index definition variable lengths exceed configured bounds");
		}

		struct BorrowedDefinition {
            uint64_t index_id = 0U;
            std::string_view name;
            IndexDefinitionEntityKind entity_kind{};
            IndexDefinitionKind kind{};
            uint64_t label_token_id = 0U;
            std::span<const std::string_view> property_names;
            uint32_t vector_dimensions = 0U;
            std::string_view vector_metric;
        } view;
        std::array<std::string_view, RSS_INDEX_DEFINITION_MAX_PROPERTIES> properties{};
		view.index_id = LittleEndianCodec::readU64(bytes, INDEX_ID_OFFSET);
		view.label_token_id = LittleEndianCodec::readU64(bytes, LABEL_TOKEN_ID_OFFSET);
		view.entity_kind = static_cast<IndexDefinitionEntityKind>(bytes[ENTITY_KIND_OFFSET]);
		view.kind = static_cast<IndexDefinitionKind>(bytes[INDEX_KIND_OFFSET]);
		view.vector_dimensions = LittleEndianCodec::readU32(bytes, VECTOR_DIMENSIONS_OFFSET);
		const std::span<const std::byte> payload = bytes.subspan(RSS_INDEX_DEFINITION_HEADER_BYTES);
		size_t offset = 0;
		view.name = readString(payload, offset, nameBytes, "relocatable index definition name is truncated");
		view.vector_metric =
				readString(payload, offset, metricBytes, "relocatable index definition vector metric is truncated");
		view.property_names = std::span<const std::string_view>(properties.data(), propertyCount);
		for (uint16_t index = 0; index < propertyCount; ++index) {
			if (payload.size() - offset < sizeof(uint16_t)) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable index definition property length is truncated");
			}
			const uint16_t propertyBytes = LittleEndianCodec::readU16(payload, offset);
			offset += sizeof(propertyBytes);
			if (propertyBytes > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable index definition property name exceeds its configured byte bound");
			}
			properties[index] = readString(payload, offset, propertyBytes, "relocatable index definition property name is truncated");
		}
		if (offset != payload.size()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable index definition payload has trailing bytes");
		}
		validateDefinition(view);
        // Finish byte/semantic validation before admitting any name allocation.
        IndexDefinition definition;
        definition.index_id = view.index_id;
        definition.name = view.name;
        definition.entity_kind = view.entity_kind;
        definition.kind = view.kind;
        definition.label_token_id = view.label_token_id;
        definition.property_names.reserve(propertyCount);
        for (size_t index = 0U; index < propertyCount; ++index)
            definition.property_names.emplace_back(properties[index]);
        definition.vector_dimensions = view.vector_dimensions;
        definition.vector_metric = view.vector_metric;
		return definition;
	}

} // namespace graph::storage
