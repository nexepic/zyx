/**
 * @file Sha256.cpp
 * @brief Dependency-free SHA-256 implementation for durable content hashes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/utils/Sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace graph::utils {

	namespace {

		constexpr std::array<uint32_t, 64U> ROUND_CONSTANTS = {
				0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
				0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
				0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
				0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
				0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
				0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
				0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
				0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U};

		[[nodiscard]] uint32_t readBigEndianU32(std::span<const std::byte, 64U> bytes, size_t offset) {
			return (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset])) << 24U) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 1U])) << 16U) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 2U])) << 8U) |
				   static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 3U]));
		}

		void transform(std::array<uint32_t, 8U> &state, std::span<const std::byte, 64U> block) {
			std::array<uint32_t, 64U> words{};
			for (size_t index = 0U; index < 16U; ++index) {
				words[index] = readBigEndianU32(block, index * 4U);
			}
			for (size_t index = 16U; index < words.size(); ++index) {
				const uint32_t s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^
									(words[index - 15U] >> 3U);
				const uint32_t s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^
									(words[index - 2U] >> 10U);
				words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
			}
			uint32_t a = state[0U];
			uint32_t b = state[1U];
			uint32_t c = state[2U];
			uint32_t d = state[3U];
			uint32_t e = state[4U];
			uint32_t f = state[5U];
			uint32_t g = state[6U];
			uint32_t h = state[7U];
			for (size_t index = 0U; index < words.size(); ++index) {
				const uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const uint32_t choose = (e & f) ^ (~e & g);
				const uint32_t temporary1 = h + s1 + choose + ROUND_CONSTANTS[index] + words[index];
				const uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t temporary2 = s0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + temporary1;
				d = c;
				c = b;
				b = a;
				a = temporary1 + temporary2;
			}
			state[0U] += a;
			state[1U] += b;
			state[2U] += c;
			state[3U] += d;
			state[4U] += e;
			state[5U] += f;
			state[6U] += g;
			state[7U] += h;
		}

	} // namespace

	Sha256::Sha256() : state_{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
								  0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U} {}

	void Sha256::update(std::span<const std::byte> bytes) {
		constexpr uint64_t MAXIMUM_INPUT_BYTES = std::numeric_limits<uint64_t>::max() / 8U;
		if (bytes.size() > MAXIMUM_INPUT_BYTES || byte_count_ > MAXIMUM_INPUT_BYTES - bytes.size()) {
			throw std::invalid_argument("SHA-256 input length exceeds the format's bit-length field");
		}
		byte_count_ += static_cast<uint64_t>(bytes.size());
		size_t consumed = 0U;
		if (tail_size_ != 0U) {
			const size_t copied = std::min(tail_.size() - tail_size_, bytes.size());
			std::copy_n(bytes.begin(), copied, tail_.begin() + static_cast<std::ptrdiff_t>(tail_size_));
			tail_size_ += copied;
			consumed = copied;
			if (tail_size_ == tail_.size()) {
				transform(state_, tail_);
				tail_size_ = 0U;
			} else {
				// The new input extends, but does not complete, the existing tail.
				return;
			}
		}
		while (bytes.size() - consumed >= tail_.size()) {
			std::array<std::byte, 64U> block{};
			std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(consumed), block.size(), block.begin());
			transform(state_, block);
			consumed += block.size();
		}
		const size_t remainder = bytes.size() - consumed;
		std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(consumed), remainder, tail_.begin());
		tail_size_ = remainder;
	}

	std::array<std::byte, SHA256_DIGEST_BYTES> Sha256::finalize() const {
		std::array<uint32_t, 8U> state = state_;
		std::array<std::byte, 128U> tail{};
		std::copy_n(tail_.begin(), tail_size_, tail.begin());
		tail[tail_size_] = std::byte{0x80};
		const size_t finalBytes = tail_size_ + 1U + sizeof(uint64_t) <= 64U ? 64U : 128U;
		const uint64_t bitLength = byte_count_ * 8U;
		for (size_t index = 0U; index < sizeof(bitLength); ++index) {
			tail[finalBytes - 1U - index] = static_cast<std::byte>((bitLength >> (index * 8U)) & UINT64_C(0xFF));
		}
		std::array<std::byte, 64U> first{};
		std::copy_n(tail.begin(), first.size(), first.begin());
		transform(state, first);
		if (finalBytes == 128U) {
			std::array<std::byte, 64U> second{};
			std::copy_n(tail.begin() + static_cast<std::ptrdiff_t>(first.size()), second.size(), second.begin());
			transform(state, second);
		}
		std::array<std::byte, SHA256_DIGEST_BYTES> result{};
		for (size_t word = 0U; word < state.size(); ++word) {
			for (size_t offset = 0U; offset < sizeof(uint32_t); ++offset) {
				result[word * sizeof(uint32_t) + offset] = static_cast<std::byte>(
						(state[word] >> ((sizeof(uint32_t) - 1U - offset) * 8U)) & UINT32_C(0xFF));
			}
		}
		return result;
	}

	std::array<std::byte, SHA256_DIGEST_BYTES> Sha256::digest(std::span<const std::byte> bytes) {
		Sha256 hasher;
		hasher.update(bytes);
		return hasher.finalize();
	}

} // namespace graph::utils
