/**
 * @file test_Sha256.cpp
 * @brief Standard-vector and padding-boundary tests for the durable SHA-256 utility.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/utils/Sha256.hpp"

namespace {

	[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
		const auto textBytes = std::as_bytes(std::span(text.data(), text.size()));
		return {textBytes.begin(), textBytes.end()};
	}

	[[nodiscard]] std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES>
	asUnsigned(std::array<std::byte, graph::utils::SHA256_DIGEST_BYTES> digest) {
		std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES> result{};
		for (size_t index = 0U; index < digest.size(); ++index) {
			result[index] = std::to_integer<uint8_t>(digest[index]);
		}
		return result;
	}

} // namespace

TEST(Sha256Test, ProducesTheStandardEmptyAndShortMessageDigests) {
	constexpr std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES> EMPTY = {
			0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U, 0x9aU, 0xfbU, 0xf4U,
			0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U, 0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU,
			0x93U, 0x4cU, 0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U};
	constexpr std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES> ABC = {
			0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU, 0x41U, 0x41U, 0x40U,
			0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U, 0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U,
			0x7aU, 0x9cU, 0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU};

	EXPECT_EQ(asUnsigned(graph::utils::Sha256::digest({})), EMPTY);
	EXPECT_EQ(asUnsigned(graph::utils::Sha256::digest(bytes("abc"))), ABC);
}

TEST(Sha256Test, ProducesTheStandardDigestAcrossTheTwoBlockPaddingBoundary) {
	constexpr std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES> EXPECTED = {
			0x24U, 0x8dU, 0x6aU, 0x61U, 0xd2U, 0x06U, 0x38U, 0xb8U, 0xe5U, 0xc0U, 0x26U,
			0x93U, 0x0cU, 0x3eU, 0x60U, 0x39U, 0xa3U, 0x3cU, 0xe4U, 0x59U, 0x64U, 0xffU,
			0x21U, 0x67U, 0xf6U, 0xecU, 0xedU, 0xd4U, 0x19U, 0xdbU, 0x06U, 0xc1U};
	const std::vector<std::byte> message = bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
	ASSERT_EQ(message.size(), 56U);
	EXPECT_EQ(asUnsigned(graph::utils::Sha256::digest(message)), EXPECTED);
}

TEST(Sha256Test, ProcessesAFullInputBlockBeforePaddingAndHandlesSubspans) {
	constexpr std::array<uint8_t, graph::utils::SHA256_DIGEST_BYTES> EXPECTED = {
			0xffU, 0xe0U, 0x54U, 0xfeU, 0x7aU, 0xe0U, 0xcbU, 0x6dU, 0xc6U, 0x5cU, 0x3aU,
			0xf9U, 0xb6U, 0x1dU, 0x52U, 0x09U, 0xf4U, 0x39U, 0x85U, 0x1dU, 0xb4U, 0x3dU,
			0x0bU, 0xa5U, 0x99U, 0x73U, 0x37U, 0xdfU, 0x15U, 0x46U, 0x68U, 0xebU};
	std::vector<std::byte> source(66U, std::byte{'x'});
	std::fill(source.begin() + 1, source.begin() + 65, std::byte{'a'});
	EXPECT_EQ(asUnsigned(graph::utils::Sha256::digest(std::span(source).subspan(1U, 64U))), EXPECTED);
}

TEST(Sha256Test, IncrementalDigestMatchesOneShotAcrossBlockAndPaddingBoundaries) {
	std::vector<std::byte> message(257U);
	for (size_t index = 0U; index < message.size(); ++index) {
		message[index] = static_cast<std::byte>(index);
	}
	graph::utils::Sha256 hasher;
	hasher.update(std::span(message).first(1U));
	hasher.update(std::span(message).subspan(1U, 63U));
	hasher.update(std::span(message).subspan(64U, 57U));
	const auto beforeFinalChunk = hasher.finalize();
	EXPECT_EQ(beforeFinalChunk, graph::utils::Sha256::digest(std::span(message).first(121U)));
	hasher.update(std::span(message).subspan(121U));
	EXPECT_EQ(hasher.finalize(), graph::utils::Sha256::digest(message));
}
