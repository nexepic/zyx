/**
 * @file Sha256.hpp
 * @brief Small dependency-free SHA-256 utility for durable content digests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace graph::utils {

	constexpr size_t SHA256_DIGEST_BYTES = 32U;

	class Sha256 final {
	public:
		Sha256();

		// Appends arbitrary-sized input while retaining at most one incomplete
		// SHA-256 block. finalize() is non-mutating so callers can snapshot a
		// digest before continuing the stream.
		void update(std::span<const std::byte> bytes);
		[[nodiscard]] std::array<std::byte, SHA256_DIGEST_BYTES> finalize() const;
		[[nodiscard]] static std::array<std::byte, SHA256_DIGEST_BYTES> digest(std::span<const std::byte> bytes);

	private:
		std::array<uint32_t, 8U> state_{};
		std::array<std::byte, 64U> tail_{};
		size_t tail_size_ = 0U;
		uint64_t byte_count_ = 0U;
	};

} // namespace graph::utils
