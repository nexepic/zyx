/**
 * @file test_PropertyCollectionRoot.cpp
 * @brief Tests for the owner-keyed relocatable Property collection-root codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include "graph/query/execution/MaterializedByteBudget.hpp"
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

TEST(RelocatablePropertyCollectionRootTest, SelectedAdmissionRunsBeforeVariablePayloadDecode) {
	const PropertyCollectionRoot source{1001U, Node::typeId,
		{{1U, PropertyValue(std::string(8192U, 'a'))}, {3U, PropertyValue()},
		 {5U, PropertyValue(std::string(4096U, 'b'))}}};
	const std::array<uint32_t, 4U> tokens{1U, 2U, 3U, 5U};
	for (const bool large: {false, true}) {
		SCOPED_TRACE(large);
		auto bytes = large ? EntityPropertyLargeValuePayloadCodec::encode(source.entries)
				: EntityPropertyCollectionRootCodec::encode(source);
		const auto decode = [&](const SelectedPropertyReadAdmission *admission) {
			return large ? EntityPropertyLargeValuePayloadCodec::decodeSelected(bytes, tokens, admission)
					: EntityPropertyCollectionRootCodec::decodeSelected(bytes, tokens, admission);
		};
		std::vector<size_t> admitted;
		const SelectedPropertyReadAdmission accept = [&](size_t size) { admitted.push_back(size); };
		const auto selected = decode(&accept);
		ASSERT_EQ(selected.size(), 4U);
		EXPECT_EQ(selected[0], std::optional{source.entries[0]});
		EXPECT_FALSE(selected[1].has_value());
		EXPECT_EQ(selected[2], std::optional{source.entries[1]});
		EXPECT_EQ(selected[3], std::optional{source.entries[2]});
		ASSERT_EQ(admitted.size(), 3U);
		EXPECT_GT(admitted[0], 8192U);
		EXPECT_LT(admitted[1], admitted[2]);
		admitted.clear();
		const auto combined = large ? EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(bytes, tokens, &accept)
			: EntityPropertyCollectionRootCodec::decodeSelectedAndCount(bytes, tokens, &accept);
		EXPECT_EQ(combined.first, source.entries.size());
		EXPECT_EQ(combined.second, selected);
		EXPECT_EQ(admitted.size(), 3U);
		const SelectedPropertyReadAdmission reject = [](size_t) { throw std::runtime_error("admission rejected"); };
		// A malformed selected value must not be decoded after admission rejects.
		const size_t valueOffset = large
				? RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES +
						EntityPropertyLargeValuePayloadCodec::decodeDirectory(bytes).front().offset + sizeof(uint32_t)
				: RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + sizeof(uint32_t) * source.entries.size() + sizeof(uint32_t);
		bytes[valueOffset] = std::byte{0xff};
		try {
			static_cast<void>(decode(&reject));
			FAIL() << "admission must reject before value decoding";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "admission rejected");
		}
		try {
			if (large) static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(bytes, tokens, &reject));
			else static_cast<void>(EntityPropertyCollectionRootCodec::decodeSelectedAndCount(bytes, tokens, &reject));
			FAIL() << "combined admission must reject before malformed payload decode";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "admission rejected");
		}
		EXPECT_THROW(static_cast<void>(decode(nullptr)), FormatException);
	}
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

TEST(RelocatablePropertyCollectionRootTest, LargeValueCombinedCountPreservesSelectionAndRejectsCorruptDirectory) {
	const auto expected = root();
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(expected.entries);
	const std::array<uint32_t, 4U> tokens{3U, 2U, 1U, 3U};
	for (int retry = 0; retry < 2; ++retry) {
		size_t calls = 0U;
		const SelectedPropertyReadAdmission admission = [&](size_t bytes) { EXPECT_GT(bytes, 0U); ++calls; };
		const auto combined = EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(payload, tokens, &admission);
		EXPECT_EQ(combined.first, expected.entries.size());
		ASSERT_EQ(combined.second.size(), tokens.size());
		EXPECT_EQ(combined.second[0], expected.entries[1]);
		EXPECT_FALSE(combined.second[1]);
		EXPECT_EQ(combined.second[2], expected.entries[0]);
		EXPECT_EQ(combined.second[3], expected.entries[1]);
		EXPECT_EQ(calls, 3U);
	}
	const auto countOnly = EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(payload, {});
	EXPECT_EQ(countOnly.first, expected.entries.size());
	EXPECT_TRUE(countOnly.second.empty());
	auto corrupt = payload;
	LittleEndianCodec::writeU32(corrupt, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES, 0U);
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(corrupt, {})), FormatException);
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

TEST(RelocatablePropertyCollectionRootTest, FullAdmissionPrecedesAllVariableDecodeAndCanRetry) {
	const PropertyCollectionRoot source{1001U, Node::typeId,
		{{1U, PropertyValue(std::string(8192U, 'a'))}, {3U, PropertyValue()},
		 {5U, PropertyValue(std::string(4096U, 'b'))}}};
	for (const bool large: {false, true}) {
		SCOPED_TRACE(large);
		auto bytes = large ? EntityPropertyLargeValuePayloadCodec::encode(source.entries) : EntityPropertyCollectionRootCodec::encode(source);
		const auto decode = [&](const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slots = nullptr) {
			return large ? EntityPropertyLargeValuePayloadCodec::decode(bytes, admission, slots)
				: EntityPropertyCollectionRootCodec::decode(bytes, admission, slots).entries;
		};
		std::vector<size_t> admitted;
		const SelectedPropertyReadAdmission accept = [&](size_t size) { admitted.push_back(size); };
		EXPECT_EQ(decode(&accept), source.entries);
		ASSERT_EQ(admitted.size(), 3U);
		EXPECT_GT(admitted[0], 8192U);
		EXPECT_GT(admitted[2], 4096U);
		const auto original = bytes;
		const size_t firstValue = large ? RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES +
			EntityPropertyLargeValuePayloadCodec::decodeDirectory(bytes).front().offset + sizeof(uint32_t)
			: RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + source.entries.size() * sizeof(uint32_t) + sizeof(uint32_t);
		bytes[firstValue] = std::byte{0xff};
		size_t callbacks = 0U;
		const SelectedPropertyReadAdmission rejectThird = [&](size_t) {
			if (++callbacks == 3U) throw std::length_error("full admission rejected");
		};
		// Rejecting the last entry must precede even the first value's decode.
		EXPECT_THROW(static_cast<void>(decode(&rejectThird)), std::length_error);
		EXPECT_EQ(callbacks, 3U);
		EXPECT_THROW(static_cast<void>(decode(nullptr)), FormatException);
		const SelectedPropertyReadAdmission rejectSlots = [](size_t) { throw std::length_error("Property slots rejected"); };
		EXPECT_THROW(static_cast<void>(decode(nullptr, &rejectSlots)), std::length_error);
		bytes = original;
		for (int retry = 0; retry < 2; ++retry) {
			std::vector<size_t> slots;
			const SelectedPropertyReadAdmission acceptSlots = [&](size_t next) { slots.push_back(next); };
			const auto output = decode(nullptr, &acceptSlots);
			EXPECT_EQ(output, source.entries);
			ASSERT_EQ(slots.size(), large ? 5U : 2U);
			EXPECT_EQ(slots.front(), source.entries.size() * (large ? sizeof(EntityPropertyLargeValueEntryLocation) : sizeof(PropertyCollectionRootEntry)));
			EXPECT_EQ(slots.back(), output.capacity() * sizeof(PropertyCollectionRootEntry));
		}
		EXPECT_EQ(decode(&accept), source.entries);
	}
}

TEST(RelocatablePropertyCollectionRootTest, FullInlineTokenValidationPrecedesAdmission) {
	const PropertyCollectionRoot source{1001U, Node::typeId, {{1U, PropertyValue(1)}, {3U, PropertyValue(2)}}};
	auto bytes = EntityPropertyCollectionRootCodec::encode(source);
	const size_t payload = RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
	const uint32_t secondOffset = LittleEndianCodec::readU32(bytes, payload + sizeof(uint32_t));
	LittleEndianCodec::writeU32(bytes, payload + secondOffset, 1U);
	size_t callbacks = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t) { ++callbacks; throw std::length_error("must not run"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decode(bytes, &reject)), FormatException);
	EXPECT_EQ(callbacks, 0U);
}

TEST(RelocatablePropertyCollectionRootTest, LargeValueDirectoryValidationPrecedesSlotRejection) {
	const std::vector<PropertyCollectionRootEntry> entries{{1U, PropertyValue()}, {3U, PropertyValue()}};
	auto bytes = EntityPropertyLargeValuePayloadCodec::encode(entries);
	LittleEndianCodec::writeU32(bytes, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES, 0U);
	size_t calls = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t) { ++calls; throw std::length_error("directory slots rejected"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(bytes, nullptr, &reject)), FormatException);
	EXPECT_EQ(calls, 0U);
}

TEST(RelocatablePropertyCollectionRootTest, LargeValueSelectedBuffersRejectBeforeValueAdmissionAndRetry) {
	const auto expected = root();
	const auto bytes = EntityPropertyLargeValuePayloadCodec::encode(expected.entries);
	const std::array<uint32_t, 3U> tokens{3U, 2U, 1U};
	size_t values = 0U;
	const SelectedPropertyReadAdmission valueAdmission = [&](size_t) { ++values; };
	const SelectedPropertyReadAdmission rejectDirectory = [](size_t) { throw std::length_error("directory rejected"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelected(bytes, tokens, &valueAdmission, &rejectDirectory)), std::length_error);
	EXPECT_EQ(values, 0U);
	const size_t directoryBytes = expected.entries.size() * sizeof(EntityPropertyLargeValueEntryLocation);
	const SelectedPropertyReadAdmission rejectResult = [&](size_t requested) { if (requested > directoryBytes) throw std::length_error("selected slots rejected"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(bytes, tokens, &valueAdmission, &rejectResult)), std::length_error);
	EXPECT_EQ(values, 0U);
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U, peak = 0U;
		const SelectedPropertyReadAdmission accept = [&](size_t requested) { retained = requested; peak = std::max(peak, requested); };
		const auto result = EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(bytes, tokens, &valueAdmission, &accept);
		EXPECT_EQ(result.first, expected.entries.size());
		EXPECT_EQ(result.second, EntityPropertyLargeValuePayloadCodec::decodeSelected(bytes, tokens));
		EXPECT_EQ(retained, result.second.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>));
		EXPECT_GE(peak, retained + directoryBytes);
	}
	EXPECT_EQ(values, 4U);
	auto corrupt = bytes;
	LittleEndianCodec::writeU32(corrupt, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES, 0U);
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeDirectory(corrupt, &rejectDirectory)), FormatException);
}

TEST(RelocatablePropertyCollectionRootTest, InlineSelectedSlotsRejectBeforeValueDecodeAndRetry) {
	const auto bytes = EntityPropertyCollectionRootCodec::encode(root());
	const std::array<uint32_t, 3U> tokens{1U, 2U, 3U};
	size_t values = 0U, retained = 0U;
	const SelectedPropertyReadAdmission payload = [&](size_t) { ++values; };
	const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("inline selected slots rejected"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeSelectedAndCount(bytes, tokens, &payload, &reject)), std::length_error);
	EXPECT_EQ(values, 0U);
	for (int retry = 0; retry < 2; ++retry) {
		const SelectedPropertyReadAdmission accept = [&](size_t count) { retained = count; };
		const auto result = EntityPropertyCollectionRootCodec::decodeSelectedAndCount(bytes, tokens, &payload, &accept);
		EXPECT_EQ(result.first, root().entries.size());
		EXPECT_EQ(result.second, EntityPropertyCollectionRootCodec::decodeSelected(bytes, tokens));
		EXPECT_EQ(retained, result.second.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>));
	}
	EXPECT_EQ(values, 4U);
}

TEST(EntityPropertyLargeValuePayloadCodecTest, PmrEncodingMatchesExistingWireForNestedKindsAndEmptyPayload) {
    PropertyValue::MapType map{{"z", PropertyValue(int64_t(7))}, {"a", PropertyValue(std::string(80, 'x'))}};
    const std::vector<PropertyCollectionRootEntry> entries{
        {1, PropertyValue(std::move(map))}, {2, PropertyValue(std::vector<PropertyValue>{PropertyValue(), PropertyValue(-0.0)})},
        {3, PropertyValue(PropertyValue::Bytes{std::byte{0}, std::byte{255}})}, {4, PropertyValue(TemporalDuration{1, 2, 3})}};
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        const auto bytes = EntityPropertyLargeValuePayloadCodec::encode(entries, *memory);
        const auto expected = EntityPropertyLargeValuePayloadCodec::encode(entries);
        EXPECT_EQ(std::vector<std::byte>(bytes.begin(), bytes.end()), expected);
        EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decode(bytes), entries);
        EXPECT_GE(memory->used(), bytes.capacity());
    }
    EXPECT_EQ(memory->used(), 0U);
    {
        const auto empty = EntityPropertyLargeValuePayloadCodec::encode({}, *memory);
        EXPECT_EQ(std::vector<std::byte>(empty.begin(), empty.end()), EntityPropertyLargeValuePayloadCodec::encode({}));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(EntityPropertyLargeValuePayloadCodecTest, PmrEncodingRejectsUnorderedTokensBeforeOutputAdmission) {
    const std::array<PropertyCollectionRootEntry, 2> duplicate{{{7, PropertyValue()}, {7, PropertyValue(true)}}};
    const std::array<PropertyCollectionRootEntry, 2> unordered{{{8, PropertyValue()}, {7, PropertyValue(true)}}};
    const std::array<PropertyCollectionRootEntry, 1> zero{{{0, PropertyValue()}}};
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    for (const auto entries : {std::span<const PropertyCollectionRootEntry>(duplicate),
        std::span<const PropertyCollectionRootEntry>(unordered), std::span<const PropertyCollectionRootEntry>(zero)})
        expectFormatError([&] { (void)EntityPropertyLargeValuePayloadCodec::encode(entries, *memory); }, FormatError::FERR_INVALID_HEADER);
    EXPECT_EQ(memory->used(), 0U);
}
TEST(EntityPropertyLargeValuePayloadCodecTest, PmrEncodingQuotaAndMidOutputCancellationRefundAllBytes) {
    const std::array<PropertyCollectionRootEntry, 2> entries{{{1, PropertyValue(std::string(900, 'a'))}, {2, PropertyValue("b")}}};
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(16);
    EXPECT_THROW((void)EntityPropertyLargeValuePayloadCodec::encode(entries, *tiny), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024); size_t checks = 0;
    EXPECT_THROW((void)EntityPropertyLargeValuePayloadCodec::encode(entries, *memory, [&] {
        if (++checks == 5) throw std::runtime_error("codec cancelled during output");
    }), std::runtime_error);
    EXPECT_EQ(checks, 5U); EXPECT_EQ(memory->used(), 0U);
    {
        const auto retry = EntityPropertyLargeValuePayloadCodec::encode(entries, *memory);
        EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decode(retry), std::vector<PropertyCollectionRootEntry>(entries.begin(), entries.end()));
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(RelocatablePropertyCollectionRootTest, BorrowedTokenDirectoryMatchesOwnedTokensAndExactEncodedValues) {
    const PropertyCollectionRoot value{91, Node::typeId, {
        {1, PropertyValue()}, {7, PropertyValue(int64_t{42})},
        {11, PropertyValue(std::string("a\0b", 3))},
        {1000, PropertyValue(std::vector<PropertyValue>{PropertyValue(true), PropertyValue("nested")})}}};
    const auto bytes = EntityPropertyCollectionRootCodec::encode(value);
    const auto owned = EntityPropertyCollectionRootCodec::decodeTokens(bytes);
    const auto view = EntityPropertyCollectionRootCodec::tokenDirectoryView(bytes);
    ASSERT_EQ(view.size(), owned.size());
    for (size_t i = 0; i < owned.size(); ++i) {
        EXPECT_EQ(view.token(i), owned[i]);
        const auto encoded = view.find(owned[i]); ASSERT_TRUE(encoded);
        std::vector<std::byte> expected;
        CanonicalPropertyValueCodec::append(expected, value.entries[i].value);
        EXPECT_EQ(std::vector<std::byte>(encoded->begin(), encoded->end()),
            expected);
    }
    EXPECT_FALSE(view.find(2)); EXPECT_FALSE(view.find(UINT32_MAX));
    EXPECT_THROW(static_cast<void>(view.find(0)), FormatException);
    EXPECT_THROW(static_cast<void>(view.token(view.size())), FormatException);
    const auto empty = EntityPropertyCollectionRootCodec::encode({91, Node::typeId, {}});
    const auto emptyView = EntityPropertyCollectionRootCodec::tokenDirectoryView(empty);
    EXPECT_EQ(emptyView.size(), 0U); EXPECT_FALSE(emptyView.find(1));
}

TEST(RelocatablePropertyCollectionRootTest, BorrowedTokenDirectoryRejectsCompleteDirectoryCorruptionAndCancellation) {
    const auto bytes = EntityPropertyCollectionRootCodec::encode(
        {91, Node::typeId, {{1, PropertyValue(int64_t{1})}, {7, PropertyValue(int64_t{2})}}});
    auto bad = bytes;
    LittleEndianCodec::writeU32(bad, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES, 0U);
    EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::tokenDirectoryView(bad)), FormatException);
    bad = bytes;
    const auto second = LittleEndianCodec::readU32(bad, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + sizeof(uint32_t));
    LittleEndianCodec::writeU32(bad, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + second, 1U);
    EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::tokenDirectoryView(bad)), FormatException);
    bad = bytes; bad.pop_back();
    EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::tokenDirectoryView(bad)), FormatException);
    class Cancel final : public CancellationCheckpoint {
    public: void check() const override { throw std::logic_error("token directory cancelled"); }
    } cancel;
    EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::tokenDirectoryView(bytes, &cancel)), std::logic_error);
}
