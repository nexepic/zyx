/**
 * @file test_ConstraintSupportTree.cpp
 * @brief Functional regression tests for internal constraint support-tree keys.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> hexDigest(std::string_view text) {
			EXPECT_EQ(text.size(), RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES * 2U);
			auto hex = [](char value) -> uint8_t {
				if (value >= '0' && value <= '9')
					return static_cast<uint8_t>(value - '0');
				if (value >= 'a' && value <= 'f')
					return static_cast<uint8_t>(value - 'a' + 10);
				return static_cast<uint8_t>(value - 'A' + 10);
			};
			std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> result{};
			for (size_t index = 0U; index < result.size(); ++index) {
				result[index] = static_cast<std::byte>((hex(text[index * 2U]) << 4U) | hex(text[index * 2U + 1U]));
			}
			return result;
		}

	} // namespace

	TEST(RelocatableConstraintSupportTreeTest, UsesStableSha256RoutingDigest) {
		const std::array<std::byte, 3U> input = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
		const auto expected = hexDigest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		EXPECT_EQ(ConstraintSupportTree::digest(input), expected);
	}

	TEST(RelocatableConstraintSupportTreeTest, EntryKeyRetainsEntityIdentityOutsideDigestPrefix) {
		const std::vector<PropertyValue> values = {PropertyValue(std::string("person@example.test"))};
		const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
		const std::vector<std::byte> first = ConstraintSupportTree::entryKey(canonical, 7U);
		const std::vector<std::byte> second = ConstraintSupportTree::entryKey(canonical, 9U);

		ASSERT_EQ(first.size(), RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES);
		EXPECT_TRUE(ConstraintSupportTree::matchesDigest(first, canonical));
		EXPECT_TRUE(ConstraintSupportTree::matchesDigest(second, canonical));
		EXPECT_EQ(ConstraintSupportTree::entryEntityId(first), 7U);
		EXPECT_EQ(ConstraintSupportTree::entryEntityId(second), 9U);
		EXPECT_NE(first, second);
	}

	TEST(RelocatableConstraintSupportTreeTest, HashesWholeBlocksAndTheTwoBlockPaddingTail) {
		std::array<std::byte, 64U> wholeBlock{};
		std::array<std::byte, 56U> twoBlockTail{};
		for (size_t index = 0U; index < wholeBlock.size(); ++index) {
			wholeBlock[index] = static_cast<std::byte>(index);
		}
		std::copy_n(wholeBlock.begin(), twoBlockTail.size(), twoBlockTail.begin());

		const auto wholeBlockDigest = ConstraintSupportTree::digest(wholeBlock);
		const auto twoBlockTailDigest = ConstraintSupportTree::digest(twoBlockTail);
		EXPECT_NE(wholeBlockDigest, twoBlockTailDigest);
	}

	TEST(RelocatableConstraintSupportTreeTest, RejectsEmptyAndOverflowedCanonicalInputs) {
		const std::vector<PropertyValue> noValues;
		const std::array<std::byte, 1U> sentinel = {std::byte{0x7F}};
		const size_t oversizedLength = static_cast<size_t>(std::numeric_limits<uint64_t>::max() / 8U + 1U);
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::canonicalKey(noValues)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::digest({})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::digest(std::span(sentinel.data(), oversizedLength))),
					 std::invalid_argument);
	}

	TEST(RelocatableConstraintSupportTreeTest, RejectsMalformedEntryKeysAndZeroEntityIds) {
		const std::vector<PropertyValue> values = {PropertyValue(int64_t{42})};
		const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
		const std::vector<std::byte> valid = ConstraintSupportTree::entryKey(canonical, 7U);
		std::vector<std::byte> mismatched = valid;
		mismatched.front() ^= std::byte{0x01};
		std::vector<std::byte> zeroEntity = valid;
		std::fill(zeroEntity.end() - static_cast<ptrdiff_t>(sizeof(uint64_t)), zeroEntity.end(), std::byte{});

		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::entryKey(canonical, 0U)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::matchesDigest({}, canonical)), std::invalid_argument);
		EXPECT_FALSE(ConstraintSupportTree::matchesDigest(mismatched, canonical));
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::entryEntityId({})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(ConstraintSupportTree::entryEntityId(zeroEntity)), std::invalid_argument);
	}

	TEST(RelocatableConstraintSupportTreeTest, LowerBoundUsesTheFirstNonzeroEntityIdentity) {
		const std::vector<PropertyValue> values = {PropertyValue(std::string("stable")), PropertyValue(int64_t{5})};
		const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
		const std::vector<std::byte> lowerBound = ConstraintSupportTree::lowerBound(canonical);

		ASSERT_EQ(lowerBound.size(), RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES);
		EXPECT_TRUE(ConstraintSupportTree::matchesDigest(lowerBound, canonical));
		EXPECT_EQ(ConstraintSupportTree::entryEntityId(lowerBound), 1U);
	}

} // namespace graph::storage
