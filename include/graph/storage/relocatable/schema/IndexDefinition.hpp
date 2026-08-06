/**
 * @file IndexDefinition.hpp
 * @brief Versioned logical schema definition for a relocatable index.
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

	constexpr uint32_t RSS_INDEX_DEFINITION_MAGIC = 0x4944595AU; // Little-endian bytes: ZYDI
	constexpr uint16_t RSS_INDEX_DEFINITION_VERSION = 1;
	constexpr uint16_t RSS_INDEX_DEFINITION_HEADER_BYTES = 48;
	constexpr uint32_t RSS_INDEX_DEFINITION_MAX_NAME_BYTES = 4096;
	constexpr uint32_t RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES = 1024;
	constexpr uint16_t RSS_INDEX_DEFINITION_MAX_PROPERTIES = 64;

	// Prefix every enumerator so Windows SDK macros cannot collide with schema values.
	enum class IndexDefinitionEntityKind : uint8_t {
		IDEK_NODE = 1,
		IDEK_EDGE = 2,
	};

	enum class IndexDefinitionKind : uint8_t {
		IDK_LABEL = 1,
		IDK_PROPERTY = 2,
		IDK_COMPOSITE = 3,
		IDK_VECTOR = 4,
	};

	// `index_id` is a public logical identifier. IDs with the high bit set are
	// reserved for metadata-owned B+Tree indexes such as schema registries.
	struct IndexDefinition {
		uint64_t index_id = 0;
		std::string name;
		IndexDefinitionEntityKind entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		IndexDefinitionKind kind = IndexDefinitionKind::IDK_LABEL;
		uint64_t label_token_id = 0;
		std::vector<std::string> property_names;
		uint32_t vector_dimensions = 0;
		std::string vector_metric;

		bool operator==(const IndexDefinition &) const = default;
	};

	class IndexDefinitionCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const IndexDefinition &definition);
		[[nodiscard]] static IndexDefinition decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
