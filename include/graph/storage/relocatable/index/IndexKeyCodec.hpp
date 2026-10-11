/**
 * @file IndexKeyCodec.hpp
 * @brief Order-preserving scalar and composite key codec for relocatable B+Tree indexes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"

namespace graph::storage {

	// Keep two variable-length entries in every index leaf. This prevents a
	// valid encoded key from making an otherwise healthy B+Tree unsplittable.
	constexpr uint32_t RSS_INDEX_KEY_MAX_BYTES =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
			 RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES -
			 2U * (RSS_SLOTTED_PAGE_SLOT_BYTES + RSS_BTREE_ENTRY_HEADER_BYTES)) /
			2U;
	static_assert(RSS_INDEX_KEY_MAX_BYTES > sizeof(uint64_t));
	constexpr uint16_t RSS_INDEX_KEY_MAX_COMPONENTS = 64;

	// This codec is deliberately separate from CanonicalPropertyValueCodec:
	// property persistence needs self-delimiting values, while B+Tree keys need
	// bytewise ordering identical to their declared scalar comparison semantics.
	class IndexKeyCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encodeValue(const PropertyValue &value);
		[[nodiscard]] static std::vector<std::byte> encodeComposite(std::span<const PropertyValue> values);
		[[nodiscard]] static std::optional<std::vector<std::byte>>
		tryEncodeComposite(std::span<const PropertyValue> values);
		[[nodiscard]] static std::vector<std::byte> encodeEntryKey(const PropertyValue &value, uint64_t entityId);
		[[nodiscard]] static std::vector<std::byte>
		encodeCompositeEntryKey(std::span<const PropertyValue> values, uint64_t entityId);
		// Oversized scalar values remain valid Properties but cannot be represented
		// in a bounded B+Tree leaf. Writers use these forms to omit only that
		// acceleration entry while preserving the authoritative Property record.
		[[nodiscard]] static std::optional<std::vector<std::byte>> tryEncodeEntryKey(const PropertyValue &value,
																		 uint64_t entityId);
		[[nodiscard]] static std::optional<std::vector<std::byte>>
		tryEncodeCompositeEntryKey(std::span<const PropertyValue> values, uint64_t entityId);
		[[nodiscard]] static std::vector<std::byte> lowerBound(const PropertyValue &value);
		[[nodiscard]] static std::vector<std::byte> upperBound(const PropertyValue &value);
		[[nodiscard]] static uint64_t decodeEntryEntityId(std::span<const std::byte> key);
	};

} // namespace graph::storage
