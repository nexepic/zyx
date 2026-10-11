/**
 * @file test_IndexDefinition.cpp
 * @brief Unit tests for the versioned relocatable logical index-definition codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	IndexDefinition labelDefinition() {
		return {1,
				"node_labels",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_LABEL,
				0,
				{},
				0,
				""};
	}

	IndexDefinition propertyDefinition() {
		return {2,
				"person_age",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_PROPERTY,
				71,
				{"age"},
				0,
				""};
	}

	IndexDefinition edgePropertyDefinition() {
		return {3,
				"edge_weight",
				IndexDefinitionEntityKind::IDEK_EDGE,
				IndexDefinitionKind::IDK_PROPERTY,
				0,
				{"weight"},
				0,
				""};
	}

	IndexDefinition compositeDefinition() {
		return {4,
				"person_name",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_COMPOSITE,
				0,
				{"family", "given"},
				0,
				""};
	}

	IndexDefinition vectorDefinition() {
		return {5,
				"person_embedding",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_VECTOR,
				71,
				{"embedding"},
				384,
				"cosine"};
	}

} // namespace

TEST(RelocatableIndexDefinitionTest, RoundTripsAllSupportedLogicalIndexKinds) {
	for (const IndexDefinition &expected:
		 {labelDefinition(), propertyDefinition(), edgePropertyDefinition(), compositeDefinition(), vectorDefinition()}) {
		const std::vector<std::byte> bytes = IndexDefinitionCodec::encode(expected);
		EXPECT_EQ(IndexDefinitionCodec::decode(bytes), expected);
	}
	IndexDefinition l2 = vectorDefinition();
	l2.index_id = 6;
	l2.vector_metric = "l2";
	EXPECT_EQ(IndexDefinitionCodec::decode(IndexDefinitionCodec::encode(l2)), l2);
	IndexDefinition ip = vectorDefinition();
	ip.index_id = 7;
	ip.vector_metric = "ip";
	EXPECT_EQ(IndexDefinitionCodec::decode(IndexDefinitionCodec::encode(ip)), ip);
}

TEST(RelocatableIndexDefinitionTest, AcceptsLabelScopedNodeCompositeDefinitions) {
	IndexDefinition definition = compositeDefinition();
	definition.label_token_id = 71U;
	EXPECT_EQ(IndexDefinitionCodec::decode(IndexDefinitionCodec::encode(definition)), definition);
}

TEST(RelocatableIndexDefinitionTest, RejectsMalformedWireHeadersAndVariableLengths) {
	const std::vector<std::byte> valid = IndexDefinitionCodec::encode(propertyDefinition());
	auto malformed = valid;
	malformed[0] = std::byte{0};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); }, FormatError::FERR_INVALID_MAGIC);

	malformed = valid;
	LittleEndianCodec::writeU16(malformed, 4, RSS_INDEX_DEFINITION_VERSION + 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_UNSUPPORTED_VERSION);

	malformed = valid;
	LittleEndianCodec::writeU16(malformed, 6, RSS_INDEX_DEFINITION_HEADER_BYTES - 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_RESERVED_BITS);

	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 44, 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_RESERVED_BITS);

	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 40, static_cast<uint32_t>(malformed.size()));
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);

	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 28, RSS_INDEX_DEFINITION_MAX_NAME_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);

	malformed = valid;
	LittleEndianCodec::writeU16(malformed, 26, RSS_INDEX_DEFINITION_MAX_PROPERTIES + 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);

	malformed = valid;
	malformed.resize(RSS_INDEX_DEFINITION_HEADER_BYTES + 1U);
	LittleEndianCodec::writeU32(malformed, 40, 1U);
	LittleEndianCodec::writeU32(malformed, 28, 0U);
	LittleEndianCodec::writeU32(malformed, 32, 0U);
	LittleEndianCodec::writeU16(malformed, 26, 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);

	malformed = valid;
	const size_t propertyLengthOffset = RSS_INDEX_DEFINITION_HEADER_BYTES + propertyDefinition().name.size();
	LittleEndianCodec::writeU16(malformed, propertyLengthOffset, RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = valid;
	LittleEndianCodec::writeU16(malformed, propertyLengthOffset, 4U);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);
	malformed = valid;
	malformed.push_back(std::byte{0xA5});
	LittleEndianCodec::writeU32(malformed, 40, static_cast<uint32_t>(malformed.size() - RSS_INDEX_DEFINITION_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);

	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 28, 0U);
	LittleEndianCodec::writeU32(malformed, 32, static_cast<uint32_t>(malformed.size()));
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);

	expectFormatError(
			[&] { static_cast<void>(IndexDefinitionCodec::decode(std::span(valid).first(4U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(RelocatableIndexDefinitionTest, RejectsInvalidLogicalDefinitions) {
	IndexDefinition invalid = propertyDefinition();
	invalid.index_id = 0;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.index_id = UINT64_C(1) << 63U;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.name.clear();
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.name.assign(RSS_INDEX_DEFINITION_MAX_NAME_BYTES + 1U, 'x');
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.entity_kind = static_cast<IndexDefinitionEntityKind>(3);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.kind = static_cast<IndexDefinitionKind>(5);
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.property_names = {"age", "age"};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = labelDefinition();
	invalid.label_token_id = 1;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.property_names.assign(RSS_INDEX_DEFINITION_MAX_PROPERTIES + 1U, "key");
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.property_names = {std::string(RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES + 1U, 'x')};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = labelDefinition();
	invalid.property_names = {"unexpected"};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.property_names.clear();
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = edgePropertyDefinition();
	invalid.label_token_id = 9;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = compositeDefinition();
	invalid.property_names = {"family"};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = vectorDefinition();
	invalid.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = vectorDefinition();
	invalid.label_token_id = 0;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = vectorDefinition();
	invalid.property_names = {"embedding", "other"};
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = vectorDefinition();
	invalid.vector_metric = "euclidean";
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = vectorDefinition();
	invalid.vector_dimensions = 0;
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = propertyDefinition();
	invalid.vector_metric = "l2";
	expectFormatError([&] { static_cast<void>(IndexDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableIndexDefinitionTest, RejectsOrExactlyRoundTripsEverySingleBitMutation) {
	IndexDefinition expected = compositeDefinition();
	expected.name = std::string("person\0index", 12);
	expected.property_names = {std::string("family\0", 7), std::string("given\xFF", 6)};
	const std::vector<std::byte> encoded = IndexDefinitionCodec::encode(expected);
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			std::vector<std::byte> mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			try {
				const IndexDefinition decoded = IndexDefinitionCodec::decode(mutation);
				EXPECT_EQ(IndexDefinitionCodec::encode(decoded), mutation)
						<< "Index definition mutation was normalized at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Any non-canonical mutation must fail at the typed format boundary.
			}
		}
	}
}
