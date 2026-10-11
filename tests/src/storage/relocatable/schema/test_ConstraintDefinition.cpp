/**
 * @file test_ConstraintDefinition.cpp
 * @brief Durable codec tests for native relocatable constraint definitions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t CONSTRAINT_ID_OFFSET = 8U;
	constexpr size_t ENTITY_KIND_OFFSET = 24U;
	constexpr size_t CONSTRAINT_KIND_OFFSET = 25U;
	constexpr size_t PROPERTY_TYPE_OFFSET = 26U;
	constexpr size_t PROPERTY_COUNT_OFFSET = 28U;
	constexpr size_t NAME_BYTES_OFFSET = 32U;
	constexpr size_t PAYLOAD_BYTES_OFFSET = 36U;
	constexpr size_t RESERVED_OFFSET = 40U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected) << error.what();
		}
	}

	ConstraintDefinition definition(std::string name, ConstraintDefinitionEntityKind entityKind,
										ConstraintDefinitionKind kind, std::vector<std::string> propertyNames,
										ConstraintDefinitionPropertyType propertyType =
												ConstraintDefinitionPropertyType::CDPT_NONE) {
		return {RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST, std::move(name), entityKind, kind, 7U, std::move(propertyNames),
				propertyType};
	}

	std::vector<std::byte> encodedUnique() {
		return ConstraintDefinitionCodec::encode(definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
				ConstraintDefinitionKind::CDK_UNIQUE, {"email"}));
	}

} // namespace

TEST(RelocatableConstraintDefinitionCodecTest, RoundTripsEveryValidConstraintShape) {
	const std::vector<ConstraintDefinition> definitions = {
			definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_UNIQUE, {"email"}),
			definition("edge_code_not_null", ConstraintDefinitionEntityKind::CDEK_EDGE,
					ConstraintDefinitionKind::CDK_NOT_NULL, {"code"}),
			definition("person_key", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_NODE_KEY, {"tenant", "subject"}),
			definition("person_boolean", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_BOOLEAN),
			definition("person_integer", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_INTEGER),
			definition("person_float", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_FLOAT),
			definition("person_string", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_STRING),
			definition("person_list", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_LIST),
			definition("person_map", ConstraintDefinitionEntityKind::CDEK_NODE,
					ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"},
					ConstraintDefinitionPropertyType::CDPT_MAP),
	};

	for (const ConstraintDefinition &current: definitions) {
		const std::vector<std::byte> bytes = ConstraintDefinitionCodec::encode(current);
		EXPECT_GE(bytes.size(), RSS_CONSTRAINT_DEFINITION_HEADER_BYTES);
		EXPECT_EQ(ConstraintDefinitionCodec::decode(bytes), current);
	}
}

TEST(RelocatableConstraintDefinitionCodecTest, RejectsInvalidDefinitionContractsBeforeWritingBytes) {
	ConstraintDefinition invalid = definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	invalid.constraint_id = 1U;
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	invalid.schema_token_id = 0U;
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid.entity_kind = static_cast<ConstraintDefinitionEntityKind>(99U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	invalid.kind = static_cast<ConstraintDefinitionKind>(99U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("person_email_unique", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	invalid.property_type = static_cast<ConstraintDefinitionPropertyType>(99U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);

	invalid = definition("", ConstraintDefinitionEntityKind::CDEK_NODE, ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition(std::string(RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES + 1U, 'n'),
			ConstraintDefinitionEntityKind::CDEK_NODE, ConstraintDefinitionKind::CDK_UNIQUE, {"email"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("too_many", ConstraintDefinitionEntityKind::CDEK_NODE, ConstraintDefinitionKind::CDK_NODE_KEY,
			std::vector<std::string>(RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES + 1U, "key"));
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("empty_property", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {""});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("long_property", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE,
			{std::string(RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U, 'p')});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("duplicate_property", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_NODE_KEY, {"same", "same"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("unique_requires_one", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"one", "two"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("node_key_requires_node", ConstraintDefinitionEntityKind::CDEK_EDGE,
			ConstraintDefinitionKind::CDK_NODE_KEY, {"key"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("type_requires_type", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_PROPERTY_TYPE, {"value"});
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = definition("unique_has_type", ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_UNIQUE, {"value"}, ConstraintDefinitionPropertyType::CDPT_STRING);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableConstraintDefinitionCodecTest, RejectsMalformedWireHeadersAndVariableLengthMetadata) {
	const std::vector<std::byte> encoded = encodedUnique();
	std::vector<std::byte> corrupt(RSS_CONSTRAINT_DEFINITION_HEADER_BYTES - 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_BUFFER_TOO_SMALL);
	corrupt = encoded;
	corrupt[0] = std::byte{0};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4U, RSS_CONSTRAINT_DEFINITION_VERSION + 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6U, 0U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	corrupt[27U] = std::byte{1};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	corrupt[30U] = std::byte{1};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	corrupt[31U] = std::byte{1};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, RESERVED_OFFSET, 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, PAYLOAD_BYTES_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, NAME_BYTES_OFFSET, RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, PROPERTY_COUNT_OFFSET, RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES + 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encoded;
	const uint32_t nameBytes = LittleEndianCodec::readU32(corrupt, NAME_BYTES_OFFSET);
	LittleEndianCodec::writeU16(corrupt, RSS_CONSTRAINT_DEFINITION_HEADER_BYTES + nameBytes,
		RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableConstraintDefinitionCodecTest, RejectsTruncatedPropertyPayloadTrailingBytesAndInvalidDecodedContracts) {
	std::vector<std::byte> corrupt = encodedUnique();
	const uint32_t nameBytes = LittleEndianCodec::readU32(corrupt, NAME_BYTES_OFFSET);
	LittleEndianCodec::writeU16(corrupt, RSS_CONSTRAINT_DEFINITION_HEADER_BYTES + nameBytes, 1024U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_BUFFER_TOO_SMALL);

	corrupt = encodedUnique();
	corrupt.push_back(std::byte{0});
	LittleEndianCodec::writeU32(corrupt, PAYLOAD_BYTES_OFFSET,
		static_cast<uint32_t>(corrupt.size() - RSS_CONSTRAINT_DEFINITION_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encodedUnique();
	LittleEndianCodec::writeU64(corrupt, CONSTRAINT_ID_OFFSET, 1U);
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encodedUnique();
	corrupt[ENTITY_KIND_OFFSET] = std::byte{99};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encodedUnique();
	corrupt[CONSTRAINT_KIND_OFFSET] = std::byte{99};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
	corrupt = encodedUnique();
	corrupt[PROPERTY_TYPE_OFFSET] = std::byte{99};
	expectFormatError([&] { static_cast<void>(ConstraintDefinitionCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableConstraintDefinitionCodecTest, RejectsOrExactlyRoundTripsEverySingleBitMutation) {
	ConstraintDefinition expected = definition(std::string("person\0key", 10), ConstraintDefinitionEntityKind::CDEK_NODE,
			ConstraintDefinitionKind::CDK_NODE_KEY, {std::string("tenant\0", 7), std::string("subject\xFF", 8)});
	const std::vector<std::byte> encoded = ConstraintDefinitionCodec::encode(expected);
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			std::vector<std::byte> mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			try {
				const ConstraintDefinition decoded = ConstraintDefinitionCodec::decode(mutation);
				EXPECT_EQ(ConstraintDefinitionCodec::encode(decoded), mutation)
						<< "Constraint definition mutation was normalized at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Any non-canonical mutation must fail at the typed format boundary.
			}
		}
	}
}
