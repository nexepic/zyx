/**
 * @file EntityLabelSet.hpp
 * @brief Canonical variable-length overflow labels for native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/entity/EntityLabelLayout.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_ENTITY_LABEL_SET_MAGIC = 0x4C45595AU; // Little-endian bytes: ZYEL
	constexpr uint16_t RSS_ENTITY_LABEL_SET_VERSION = 1;
	constexpr uint16_t RSS_ENTITY_LABEL_SET_HEADER_BYTES = 24;
	constexpr size_t RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT = alignof(uint64_t);
	constexpr size_t RSS_ENTITY_LABEL_SET_AVAILABLE_RECORD_BYTES =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;

	[[nodiscard]] constexpr size_t alignEntityLabelSetRecordBytes(size_t bytes) noexcept {
		return (bytes + RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT - 1U) &
			   ~(RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT - 1U);
	}

	static_assert(RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT != 0U);
static_assert((RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT & (RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT - 1U)) == 0U);
static_assert(RSS_ENTITY_LABEL_SET_HEADER_BYTES % RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT == 0U);
static_assert(RSS_ENTITY_LABEL_TOKEN_BYTES % RSS_ENTITY_LABEL_SET_RECORD_ALIGNMENT == 0U);
	static_assert(RSS_ENTITY_LABEL_SET_AVAILABLE_RECORD_BYTES >=
			  alignEntityLabelSetRecordBytes(RSS_ENTITY_LABEL_SET_HEADER_BYTES));

	// Derived, non-persisted limits. The codec and tests consume these expressions
	// so a base-layout change cannot leave a copied capacity result behind.
	constexpr size_t RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS =
			(RSS_ENTITY_LABEL_SET_AVAILABLE_RECORD_BYTES -
			 alignEntityLabelSetRecordBytes(RSS_ENTITY_LABEL_SET_HEADER_BYTES)) /
			RSS_ENTITY_LABEL_TOKEN_BYTES;
	constexpr size_t RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES = alignEntityLabelSetRecordBytes(
			RSS_ENTITY_LABEL_SET_HEADER_BYTES + RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS * RSS_ENTITY_LABEL_TOKEN_BYTES);
	constexpr size_t RSS_ENTITY_MAX_LABELS = RSS_ENTITY_INLINE_LABEL_CAPACITY + RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS;

static_assert(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS > 0U);
static_assert(RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES <= RSS_ENTITY_LABEL_SET_AVAILABLE_RECORD_BYTES);
static_assert(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS <= std::numeric_limits<uint32_t>::max());
static_assert(RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES - RSS_ENTITY_LABEL_SET_HEADER_BYTES <=
			  std::numeric_limits<uint32_t>::max());

	// An overflow slot stores only labels after the Node record's inline prefix.
	// The owner Node ID is deliberately not repeated: the Node's stable
	// PageId/slot/generation reference is the sole ownership edge.
	class EntityLabelSetCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(std::span<const uint64_t> labelIds);
		[[nodiscard]] static std::vector<uint64_t> decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
