/**
 * @file TokenDefinition.hpp
 * @brief Versioned logical Token definition used by relocatable schema metadata.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_TOKEN_DEFINITION_MAGIC = 0x4B54595AU; // Little-endian bytes: ZYTK
	constexpr uint16_t RSS_TOKEN_DEFINITION_VERSION = 1;
	constexpr uint16_t RSS_TOKEN_DEFINITION_HEADER_BYTES = 24;
	constexpr uint32_t RSS_TOKEN_DEFINITION_MAX_NAME_BYTES = 4096;

	struct TokenDefinition {
		uint64_t token_id = 0;
		std::string name;

		bool operator==(const TokenDefinition &) const = default;
	};

	class TokenDefinitionCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const TokenDefinition &definition);
		[[nodiscard]] static TokenDefinition decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
