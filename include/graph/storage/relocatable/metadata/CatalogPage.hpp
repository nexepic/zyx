/**
 * @file CatalogPage.hpp
 * @brief Explicit fixed-size codec for a relocatable Catalog leaf metadata page.
 *
 * The structs below are in-memory values only. CatalogLeafPageCodec is the
 * sole definition of their persisted little-endian representation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/metadata/Catalog.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_CATALOG_PAGE_MAGIC = 0x4358595AU; // Little-endian bytes: ZYXC
	constexpr uint16_t RSS_CATALOG_PAGE_VERSION = 4;
	constexpr uint32_t RSS_CATALOG_PAGE_HEADER_BYTES = 64;
	constexpr uint32_t RSS_CATALOG_ENTRY_BYTES = 48;
	constexpr uint32_t RSS_CATALOG_PAGE_MAX_ENTRIES =
			(RSS_PAGE_SIZE - RSS_CATALOG_PAGE_HEADER_BYTES) / RSS_CATALOG_ENTRY_BYTES;
	static_assert(RSS_CATALOG_PAGE_HEADER_BYTES + RSS_CATALOG_PAGE_MAX_ENTRIES * RSS_CATALOG_ENTRY_BYTES ==
				  RSS_PAGE_SIZE);

	// A leaf is range-routed by a future Catalog radix root; it never uses a segment link.
	struct CatalogLeafPage {
		PageId page_id{};
		SegmentId minimum_segment_id{};
		SegmentId maximum_segment_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		std::vector<CatalogEntry> entries;
	};

	class CatalogLeafPageCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_PAGE_SIZE> encode(const CatalogLeafPage &page);
		[[nodiscard]] static CatalogLeafPage decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
