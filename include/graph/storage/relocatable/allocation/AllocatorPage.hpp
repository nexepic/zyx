/**
 * @file AllocatorPage.hpp
 * @brief Explicit fixed-size codec for a relocatable physical-slot bitmap leaf.
 *
 * The in-memory value is never persisted through its C++ representation.
 * AllocatorLeafPageCodec is the sole definition of its little-endian disk
 * format. A later copy-on-write radix directory will route to these leaves.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_ALLOCATOR_PAGE_MAGIC = 0x4158595AU; // Little-endian bytes: ZYXA
	constexpr uint16_t RSS_ALLOCATOR_PAGE_VERSION = 1;
	constexpr uint32_t RSS_ALLOCATOR_PAGE_HEADER_BYTES = 64;
	constexpr uint32_t RSS_ALLOCATOR_PAGE_BITMAP_BYTES = RSS_PAGE_SIZE - RSS_ALLOCATOR_PAGE_HEADER_BYTES;
	constexpr uint32_t RSS_ALLOCATOR_PAGE_SLOT_CAPACITY = RSS_ALLOCATOR_PAGE_BITMAP_BYTES * 8U;

	// Each bit represents one physical 128 KiB slot in a contiguous range. A set
	// bit means the slot is allocated or reserved; clear means free.
	struct AllocatorLeafPage {
		PageId page_id{};
		PhysicalSlotId first_slot{};
		uint32_t slot_count = 0;
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		std::array<std::byte, RSS_ALLOCATOR_PAGE_BITMAP_BYTES> allocated_slots{};
	};

	class AllocatorLeafPageCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_PAGE_SIZE> encode(const AllocatorLeafPage &page);
		[[nodiscard]] static AllocatorLeafPage decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
