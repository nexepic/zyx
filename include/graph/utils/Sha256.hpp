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
#include <span>

namespace graph::utils {

	constexpr size_t SHA256_DIGEST_BYTES = 32U;

	class Sha256 final {
	public:
		[[nodiscard]] static std::array<std::byte, SHA256_DIGEST_BYTES> digest(std::span<const std::byte> bytes);
	};

} // namespace graph::utils
