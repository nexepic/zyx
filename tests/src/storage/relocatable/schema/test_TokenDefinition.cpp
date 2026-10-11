/**
 * @file test_TokenDefinition.cpp
 * @brief Unit tests for the versioned relocatable Token-definition codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/schema/TokenDefinition.hpp"

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

} // namespace

TEST(RelocatableTokenDefinitionTest, RoundTripsBinarySafeTokenNames) {
	const TokenDefinition expected{71, std::string("per\0son", 7)};
	const std::vector<std::byte> bytes = TokenDefinitionCodec::encode(expected);
	EXPECT_EQ(TokenDefinitionCodec::decode(bytes), expected);
}

TEST(RelocatableTokenDefinitionTest, RejectsMalformedWireHeadersAndLengths) {
	const std::vector<std::byte> valid = TokenDefinitionCodec::encode({1, "Person"});
	auto malformed = valid;
	malformed[0] = std::byte{0};
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); }, FormatError::FERR_INVALID_MAGIC);
	malformed = valid;
	LittleEndianCodec::writeU16(malformed, 4, RSS_TOKEN_DEFINITION_VERSION + 1U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_UNSUPPORTED_VERSION);
	malformed = valid;
	LittleEndianCodec::writeU16(malformed, 6, RSS_TOKEN_DEFINITION_HEADER_BYTES - 1U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_RESERVED_BITS);
	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 20, 1U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_RESERVED_BITS);
	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 16, RSS_TOKEN_DEFINITION_MAX_NAME_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 16, static_cast<uint32_t>(valid.size()));
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);
	malformed = valid;
	LittleEndianCodec::writeU32(malformed, 16, 1U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(TokenDefinitionCodec::decode(std::span(valid).first(4U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(RelocatableTokenDefinitionTest, RejectsInvalidLogicalDefinitions) {
	TokenDefinition invalid{0, "Person"};
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = {1, ""};
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);
	invalid = {1, std::string(RSS_TOKEN_DEFINITION_MAX_NAME_BYTES + 1U, 'x')};
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::encode(invalid)); }, FormatError::FERR_INVALID_HEADER);

	auto malformed = TokenDefinitionCodec::encode({1, "Person"});
	LittleEndianCodec::writeU64(malformed, 8, 0U);
	expectFormatError([&] { static_cast<void>(TokenDefinitionCodec::decode(malformed)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableTokenDefinitionTest, RejectsOrExactlyRoundTripsEverySingleBitMutation) {
	std::string name{"token", 5};
	name.push_back('\0');
	name.append("name", 4);
	name.push_back(static_cast<char>(0xFF));
	const std::vector<std::byte> encoded = TokenDefinitionCodec::encode({71U, name});
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			std::vector<std::byte> mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			try {
				const TokenDefinition decoded = TokenDefinitionCodec::decode(mutation);
				EXPECT_EQ(TokenDefinitionCodec::encode(decoded), mutation)
						<< "Token definition mutation was normalized at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Any non-canonical mutation must fail at the typed format boundary.
			}
		}
	}
}
