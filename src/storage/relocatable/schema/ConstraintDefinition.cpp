/**
 * @file ConstraintDefinition.cpp
 * @brief Explicit bounded codec for native relocatable constraint definitions.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"

#include <array>
#include <limits>
#include <set>
#include <string_view>

namespace graph::storage {

	namespace {

		constexpr size_t CONSTRAINT_ID_OFFSET = 8U;
		constexpr size_t SCHEMA_TOKEN_ID_OFFSET = 16U;
		constexpr size_t ENTITY_KIND_OFFSET = 24U;
		constexpr size_t CONSTRAINT_KIND_OFFSET = 25U;
		constexpr size_t PROPERTY_TYPE_OFFSET = 26U;
		constexpr size_t PROPERTY_COUNT_OFFSET = 28U;
		constexpr size_t NAME_BYTES_OFFSET = 32U;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 36U;
		constexpr size_t RESERVED_OFFSET = 40U;
		constexpr uint64_t CONSTRAINT_ID_TYPE_MASK = UINT64_C(0xE000000000000000);
		constexpr uint64_t CONSTRAINT_ID_TYPE_PREFIX = UINT64_C(0xC000000000000000);

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isNativeConstraintId(uint64_t id) {
			return (id & CONSTRAINT_ID_TYPE_MASK) == CONSTRAINT_ID_TYPE_PREFIX;
		}

		[[nodiscard]] bool isKnownEntityKind(ConstraintDefinitionEntityKind kind) {
			return kind == ConstraintDefinitionEntityKind::CDEK_NODE || kind == ConstraintDefinitionEntityKind::CDEK_EDGE;
		}

		[[nodiscard]] bool isKnownConstraintKind(ConstraintDefinitionKind kind) {
			return kind == ConstraintDefinitionKind::CDK_UNIQUE || kind == ConstraintDefinitionKind::CDK_NOT_NULL ||
				   kind == ConstraintDefinitionKind::CDK_PROPERTY_TYPE ||
				   kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		}

		[[nodiscard]] bool isKnownPropertyType(ConstraintDefinitionPropertyType type) {
			return type == ConstraintDefinitionPropertyType::CDPT_NONE ||
				   type == ConstraintDefinitionPropertyType::CDPT_BOOLEAN ||
				   type == ConstraintDefinitionPropertyType::CDPT_INTEGER ||
				   type == ConstraintDefinitionPropertyType::CDPT_FLOAT ||
				   type == ConstraintDefinitionPropertyType::CDPT_STRING ||
				   type == ConstraintDefinitionPropertyType::CDPT_LIST ||
				   type == ConstraintDefinitionPropertyType::CDPT_MAP;
		}

		void validateByteString(std::string_view value, uint32_t maximum, const char *message) {
			if (value.empty() || value.size() > maximum) {
				fail(FormatError::FERR_INVALID_HEADER, message);
			}
		}

		void validateDefinition(const ConstraintDefinition &definition) {
			if (!isNativeConstraintId(definition.constraint_id) || definition.schema_token_id == 0U ||
				!isKnownEntityKind(definition.entity_kind) || !isKnownConstraintKind(definition.kind) ||
				!isKnownPropertyType(definition.property_type)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable constraint definition has an invalid identity, schema Token, or enum value");
			}
			validateByteString(definition.name, RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES,
							   "relocatable constraint definition name is empty or exceeds its byte bound");
			if (definition.property_names.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable constraint definition exceeds the supported Property-key component count");
			}
			std::set<std::string, std::less<>> uniqueNames;
			for (const std::string &property: definition.property_names) {
				validateByteString(property, RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES,
							   "relocatable constraint definition Property name is empty or exceeds its byte bound");
				if (!uniqueNames.insert(property).second) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable constraint definition repeats a Property-key component");
				}
			}
			const bool oneProperty = definition.property_names.size() == 1U;
			const uint8_t invalidContract =
					static_cast<uint8_t>((definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
									  definition.kind == ConstraintDefinitionKind::CDK_NOT_NULL ||
									  definition.kind == ConstraintDefinitionKind::CDK_PROPERTY_TYPE) &&
									 !oneProperty) |
					static_cast<uint8_t>(definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY &&
									 (definition.entity_kind != ConstraintDefinitionEntityKind::CDEK_NODE ||
									  definition.property_names.empty())) |
					static_cast<uint8_t>(definition.kind == ConstraintDefinitionKind::CDK_PROPERTY_TYPE &&
									 definition.property_type == ConstraintDefinitionPropertyType::CDPT_NONE) |
					static_cast<uint8_t>(definition.kind != ConstraintDefinitionKind::CDK_PROPERTY_TYPE &&
									 definition.property_type != ConstraintDefinitionPropertyType::CDPT_NONE);
			if (invalidContract != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable constraint definition has an invalid kind-specific schema contract");
			}
		}

		void appendU16(std::vector<std::byte> &bytes, uint16_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU16(bytes, offset, value);
		}

		void appendBytes(std::vector<std::byte> &bytes, std::string_view value) {
			if (!value.empty()) {
				const std::byte *begin = reinterpret_cast<const std::byte *>(value.data());
				bytes.insert(bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(value.size()));
			}
		}

		[[nodiscard]] std::string readString(std::span<const std::byte> payload, size_t &offset, size_t length,
												const char *message) {
			if (length > payload.size() - offset) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, message);
			}
			const char *data = reinterpret_cast<const char *>(payload.data() + offset);
			offset += length;
			return {data, length};
		}

	} // namespace

	std::vector<std::byte> ConstraintDefinitionCodec::encode(const ConstraintDefinition &definition) {
		validateDefinition(definition);
		std::vector<std::byte> payload;
		payload.reserve(definition.name.size() + definition.property_names.size() * (sizeof(uint16_t) + 16U));
		appendBytes(payload, definition.name);
		for (const std::string &property: definition.property_names) {
			appendU16(payload, static_cast<uint16_t>(property.size()));
			appendBytes(payload, property);
		}
		if (payload.size() > std::numeric_limits<uint32_t>::max()) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable constraint definition payload exceeds its wire capacity");
		}
		std::vector<std::byte> bytes(RSS_CONSTRAINT_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, 0U, RSS_CONSTRAINT_DEFINITION_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, RSS_CONSTRAINT_DEFINITION_VERSION);
		LittleEndianCodec::writeU16(bytes, 6U, RSS_CONSTRAINT_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes, CONSTRAINT_ID_OFFSET, definition.constraint_id);
		LittleEndianCodec::writeU64(bytes, SCHEMA_TOKEN_ID_OFFSET, definition.schema_token_id);
		bytes[ENTITY_KIND_OFFSET] = static_cast<std::byte>(definition.entity_kind);
		bytes[CONSTRAINT_KIND_OFFSET] = static_cast<std::byte>(definition.kind);
		bytes[PROPERTY_TYPE_OFFSET] = static_cast<std::byte>(definition.property_type);
		LittleEndianCodec::writeU16(bytes, PROPERTY_COUNT_OFFSET,
									 static_cast<uint16_t>(definition.property_names.size()));
		LittleEndianCodec::writeU32(bytes, NAME_BYTES_OFFSET, static_cast<uint32_t>(definition.name.size()));
		LittleEndianCodec::writeU32(bytes, PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payload.size()));
		LittleEndianCodec::writeU64(bytes, RESERVED_OFFSET, 0U);
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return bytes;
	}

	ConstraintDefinition ConstraintDefinitionCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_CONSTRAINT_DEFINITION_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable constraint definition is truncated");
		}
		if (LittleEndianCodec::readU32(bytes, 0U) != RSS_CONSTRAINT_DEFINITION_MAGIC) {
			fail(FormatError::FERR_INVALID_MAGIC, "relocatable constraint definition magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, 4U) != RSS_CONSTRAINT_DEFINITION_VERSION) {
			fail(FormatError::FERR_UNSUPPORTED_VERSION, "relocatable constraint definition version is unsupported");
		}
		if (LittleEndianCodec::readU16(bytes, 6U) != RSS_CONSTRAINT_DEFINITION_HEADER_BYTES ||
			LittleEndianCodec::readU64(bytes, RESERVED_OFFSET) != 0U) {
			fail(FormatError::FERR_RESERVED_BITS,
				 "relocatable constraint definition fixed or reserved fields are invalid");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_CONSTRAINT_DEFINITION_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable constraint definition payload length does not match its bytes");
		}
		const uint32_t nameBytes = LittleEndianCodec::readU32(bytes, NAME_BYTES_OFFSET);
		const uint16_t propertyCount = LittleEndianCodec::readU16(bytes, PROPERTY_COUNT_OFFSET);
		if (nameBytes > RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES ||
			propertyCount > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES || nameBytes > payloadBytes) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable constraint definition variable lengths exceed configured bounds");
		}

		ConstraintDefinition definition;
		definition.constraint_id = LittleEndianCodec::readU64(bytes, CONSTRAINT_ID_OFFSET);
		definition.schema_token_id = LittleEndianCodec::readU64(bytes, SCHEMA_TOKEN_ID_OFFSET);
		definition.entity_kind = static_cast<ConstraintDefinitionEntityKind>(bytes[ENTITY_KIND_OFFSET]);
		definition.kind = static_cast<ConstraintDefinitionKind>(bytes[CONSTRAINT_KIND_OFFSET]);
		definition.property_type = static_cast<ConstraintDefinitionPropertyType>(bytes[PROPERTY_TYPE_OFFSET]);
		const std::span<const std::byte> payload = bytes.subspan(RSS_CONSTRAINT_DEFINITION_HEADER_BYTES);
		size_t offset = 0U;
		definition.name = readString(payload, offset, nameBytes, "relocatable constraint definition name is truncated");
		definition.property_names.reserve(propertyCount);
		for (uint16_t index = 0U; index < propertyCount; ++index) {
			if (payload.size() - offset < sizeof(uint16_t)) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL,
					 "relocatable constraint definition Property length is truncated");
			}
			const uint16_t propertyBytes = LittleEndianCodec::readU16(payload, offset);
			offset += sizeof(propertyBytes);
			if (propertyBytes > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable constraint definition Property name exceeds its configured byte bound");
			}
			definition.property_names.push_back(readString(
					payload, offset, propertyBytes, "relocatable constraint definition Property name is truncated"));
		}
		if (offset != payload.size()) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable constraint definition payload has trailing bytes");
		}
		validateDefinition(definition);
		return definition;
	}

} // namespace graph::storage
