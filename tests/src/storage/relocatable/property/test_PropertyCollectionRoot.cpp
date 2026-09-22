/**
 * @file test_PropertyCollectionRoot.cpp
 * @brief Tests for the owner-keyed relocatable Property collection-root codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <functional>
#include <optional>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;
using graph::TemporalDuration;

namespace {

	constexpr size_t MAGIC_OFFSET = 0U;
	constexpr size_t VERSION_OFFSET = 4U;
	constexpr size_t HEADER_BYTES_OFFSET = 6U;
	constexpr size_t ENTITY_OWNER_TYPE_OFFSET = 16U;
	constexpr size_t ENTITY_ENTRY_COUNT_OFFSET = 20U;
	constexpr size_t ENTITY_PAYLOAD_BYTES_OFFSET = 24U;
	constexpr size_t ENTITY_RESERVED_OFFSET = 28U;
	constexpr size_t LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET = 8U;
	constexpr size_t LARGE_VALUE_PAYLOAD_BYTES_OFFSET = 12U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	PropertyCollectionRoot root() {
		PropertyCollectionRoot value;
		value.owner_entity_id = 97U;
		value.owner_entity_type = Node::typeId;
		value.entries = {
				{1U, PropertyValue(int64_t(-44))},
				{3U, PropertyValue(PropertyValue::MapType{{"first", PropertyValue(true)},
															 {"second", PropertyValue(TemporalDuration{1, -2, 3})}})}};
		return value;
	}

} // namespace

TEST(RelocatablePropertyCollectionRootTest, RoundTripsCanonicalOwnerKeyedCollections) {
	const PropertyCollectionRoot expected = root();
	const std::vector<std::byte> nodeBytes = EntityPropertyCollectionRootCodec::encode(expected);
	EXPECT_EQ(LittleEndianCodec::readU16(nodeBytes, HEADER_BYTES_OFFSET),
			  RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(nodeBytes), expected);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::encode(EntityPropertyCollectionRootCodec::decode(nodeBytes)), nodeBytes);

	PropertyCollectionRoot edgeOwned = expected;
	edgeOwned.owner_entity_type = Edge::typeId;
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(EntityPropertyCollectionRootCodec::encode(edgeOwned)), edgeOwned);
}

TEST(RelocatablePropertyCollectionRootTest, SelectiveDecodeReturnsOnlyTheRequestedToken) {
	const std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root());
	const auto selected = EntityPropertyCollectionRootCodec::decodeSelected(encoded, 3U);
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(selected->key_token, 3U);
	EXPECT_EQ(selected->value.getType(), graph::PropertyType::MAP);
	EXPECT_FALSE(EntityPropertyCollectionRootCodec::decodeSelected(encoded, 2U).has_value());
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeSelected(encoded, 0U)), FormatException);
}

TEST(RelocatablePropertyCollectionRootTest, PresenceCodecUsesCanonicalDirectoryWithoutMaterializingValues) {
	const PropertyCollectionRoot expected = root();
	const std::vector<std::byte> rootBytes = EntityPropertyCollectionRootCodec::encode(expected);
	EXPECT_TRUE(EntityPropertyCollectionRootCodec::containsToken(rootBytes, 1U));
	EXPECT_TRUE(EntityPropertyCollectionRootCodec::containsToken(rootBytes, 3U));
	EXPECT_FALSE(EntityPropertyCollectionRootCodec::containsToken(rootBytes, 2U));
	EXPECT_FALSE(EntityPropertyCollectionRootCodec::containsToken(rootBytes, 99U));
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::containsToken(rootBytes, 0U)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::vector<std::byte> payloadBytes = EntityPropertyLargeValuePayloadCodec::encode(expected.entries);
	EXPECT_TRUE(EntityPropertyLargeValuePayloadCodec::containsToken(payloadBytes, 3U));
	EXPECT_FALSE(EntityPropertyLargeValuePayloadCodec::containsToken(payloadBytes, 2U));
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::containsToken(payloadBytes, 0U)); },
					  FormatError::FERR_INVALID_HEADER);

	auto malformedRoot = rootBytes;
	LittleEndianCodec::writeU32(malformedRoot, ENTITY_ENTRY_COUNT_OFFSET, 1000U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::containsToken(malformedRoot, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	malformedRoot = rootBytes;
	LittleEndianCodec::writeU32(malformedRoot, ENTITY_OWNER_TYPE_OFFSET, 99U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::containsToken(malformedRoot, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	auto malformedPayload = payloadBytes;
	LittleEndianCodec::writeU32(malformedPayload, LARGE_VALUE_PAYLOAD_BYTES_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::containsToken(malformedPayload, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePropertyCollectionRootTest, MultiColumnSelectiveDecodePreservesTokenOrderAndAbsence) {
	const std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root());
	const std::array<uint32_t, 3U> tokens{1U, 2U, 3U};
	const std::vector<std::optional<PropertyCollectionRootEntry>> selected =
			EntityPropertyCollectionRootCodec::decodeSelected(encoded, tokens);
	ASSERT_EQ(selected.size(), tokens.size());
	ASSERT_TRUE(selected[0].has_value());
	EXPECT_EQ(*selected[0], root().entries[0]);
	EXPECT_FALSE(selected[1].has_value());
	ASSERT_TRUE(selected[2].has_value());
	EXPECT_EQ(*selected[2], root().entries[1]);

	const std::vector<std::byte> payload = EntityPropertyLargeValuePayloadCodec::encode(root().entries);
	EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decodeSelected(payload, tokens), selected);
	const std::array<uint32_t, 2U> duplicate{1U, 1U};
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decodeSelected(encoded, duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePropertyCollectionRootTest, SelectiveDecodersValidateOwnerNeutralAndOwnerKeyedPayloads) {
	const PropertyCollectionRoot expected = root();
	const std::vector<std::byte> rootBytes = EntityPropertyCollectionRootCodec::encode(expected);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeOwner(rootBytes).owner_entity_id, expected.owner_entity_id);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeOwner(rootBytes).owner_entity_type, expected.owner_entity_type);

	const std::vector<std::byte> payload = EntityPropertyLargeValuePayloadCodec::encode(expected.entries);
	const auto selected = EntityPropertyLargeValuePayloadCodec::decodeSelected(payload, 1U);
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(*selected, expected.entries.front());
	EXPECT_FALSE(EntityPropertyLargeValuePayloadCodec::decodeSelected(payload, 2U).has_value());
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelected(payload, 0U)), FormatException);

	auto malformedRoot = rootBytes;
	LittleEndianCodec::writeU32(malformedRoot, ENTITY_OWNER_TYPE_OFFSET, 99U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decodeSelected(malformedRoot, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	auto malformedPayload = payload;
	LittleEndianCodec::writeU32(malformedPayload, LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET, 1000U);
	expectFormatError(
			[&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelected(malformedPayload, 1U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePropertyCollectionRootTest, RoundTripsOwnerNeutralLargeValuePayloads) {
	const PropertyCollectionRoot ownerA = root();
	PropertyCollectionRoot ownerB = ownerA;
	ownerB.owner_entity_id = 98U;
	ownerB.owner_entity_type = Edge::typeId;
	const std::vector<std::byte> encoded = EntityPropertyLargeValuePayloadCodec::encode(ownerA.entries);
	EXPECT_TRUE(EntityPropertyLargeValuePayloadCodec::hasMagic(encoded));
	EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decode(encoded), ownerA.entries);
	EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::encode(ownerB.entries), encoded);

	expectFormatError(
			[&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(std::span(encoded).first(15U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	auto malformed = encoded;
	LittleEndianCodec::writeU32(malformed, MAGIC_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, LARGE_VALUE_PAYLOAD_BYTES_OFFSET, 1U);
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET, 1000U);
	expectFormatError([&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			try {
				const std::vector<PropertyCollectionRootEntry> decoded =
						EntityPropertyLargeValuePayloadCodec::decode(mutation);
				EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::encode(decoded), mutation)
						<< "mutation at byte " << byteOffset << ", bit " << static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Invalid images must stop at the typed LargeValue payload boundary.
			}
		}
	}
}

TEST(RelocatablePropertyCollectionRootTest, RejectsInvalidOwnerIdentitiesAndTokenOrder) {
	PropertyCollectionRoot invalid = root();
	invalid.owner_entity_id = 0U;
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = root();
	invalid.owner_entity_type = 99U;
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = root();
	invalid.entries.front().key_token = 0U;
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = root();
	invalid.entries.back().key_token = 1U;
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<PropertyCollectionRootEntry> invalidPayloadEntries = root().entries;
	invalidPayloadEntries.front().key_token = 0U;
	expectFormatError(
			[&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::encode(invalidPayloadEntries)); },
			FormatError::FERR_INVALID_HEADER);
	invalidPayloadEntries = root().entries;
	invalidPayloadEntries.back().key_token = 1U;
	expectFormatError(
			[&] { static_cast<void>(EntityPropertyLargeValuePayloadCodec::encode(invalidPayloadEntries)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePropertyCollectionRootTest, FailsClosedForMalformedPersistentBytes) {
	const std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root());
	expectFormatError(
			[&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(std::span(encoded).first(31U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	auto malformed = encoded;
	LittleEndianCodec::writeU32(malformed, MAGIC_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_MAGIC);
	malformed = encoded;
	LittleEndianCodec::writeU16(malformed, VERSION_OFFSET, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION + 1U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	malformed = encoded;
	LittleEndianCodec::writeU16(malformed, HEADER_BYTES_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_RESERVED_BITS);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, ENTITY_RESERVED_OFFSET, 1U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_RESERVED_BITS);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, ENTITY_PAYLOAD_BYTES_OFFSET, 1U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, ENTITY_OWNER_TYPE_OFFSET, 99U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, ENTITY_ENTRY_COUNT_OFFSET, 1000U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES, 0U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	const size_t secondTokenOffset = LittleEndianCodec::readU32(
			malformed, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + sizeof(uint32_t));
	LittleEndianCodec::writeU32(malformed, secondTokenOffset, 1U);
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed.push_back(std::byte{0U});
	LittleEndianCodec::writeU32(malformed, ENTITY_PAYLOAD_BYTES_OFFSET,
							 static_cast<uint32_t>(malformed.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(EntityPropertyCollectionRootCodec::decode(malformed)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatablePropertyCollectionRootTest, RejectsOrExactlyRoundTripsEverySingleBitMutation) {
	const std::vector<std::byte> encoded = EntityPropertyCollectionRootCodec::encode(root());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			try {
				const PropertyCollectionRoot decoded = EntityPropertyCollectionRootCodec::decode(mutation);
				EXPECT_EQ(EntityPropertyCollectionRootCodec::encode(decoded), mutation)
						<< "mutation at byte " << byteOffset << ", bit " << static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// Invalid images must stop at the typed Property-root boundary.
			}
		}
	}
}
