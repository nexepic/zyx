/**
 * @file HybridPropertyIndexIds.hpp
 * @brief Reserved namespace identities for the isolated hybrid descriptor.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {
constexpr size_t RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT = RSS_SCAN_PATH_FORMAT_ENABLED ? 9 : 8;
enum class HybridPropertyRootOrdinal : uint8_t {
    HPRO_NODE_OWNER = 0, HPRO_NODE_COLUMN, HPRO_NODE_EXTENSION, HPRO_NODE_GROUP,
    HPRO_EDGE_OWNER, HPRO_EDGE_COLUMN, HPRO_EDGE_EXTENSION, HPRO_EDGE_GROUP,
    HPRO_NODE_SCAN,
};
// Disjoint from schema definition IDs [2^62,2^63), global adjacency ...0002,
// system catalog ...0020, vector A501..A504 prefixes and the large-value UINT64_MAX.
// These identities are reserved in every build; reservation does not admit a
// field root or remove the isolated format's nonempty-descriptor guards.
constexpr uint64_t RSS_HYBRID_PROPERTY_INDEX_ID_FIRST = UINT64_C(0x8000000000000030);
[[nodiscard]] constexpr uint64_t hybridPropertyIndexId(HybridPropertyRootOrdinal ordinal) {
    const auto offset = static_cast<uint8_t>(ordinal);
    if (offset >= RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT)
        throw FormatException(FormatError::FERR_INVALID_HEADER, "invalid hybrid property root namespace");
    return RSS_HYBRID_PROPERTY_INDEX_ID_FIRST + offset;
}
[[nodiscard]] constexpr bool isHybridPropertyIndexId(uint64_t id) noexcept {
    return id >= RSS_HYBRID_PROPERTY_INDEX_ID_FIRST &&
        id - RSS_HYBRID_PROPERTY_INDEX_ID_FIRST < 9U;
}
} // namespace graph::storage
