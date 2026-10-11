/**
 * @file MetadataPageRoute.hpp
 * @brief Typed physical routes for stable metadata COW page identities.
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

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_METADATA_PAGE_ROUTE_LEAF_MAGIC = 0x4D58595AU; // Little-endian bytes: ZYXM
	constexpr uint16_t RSS_METADATA_PAGE_ROUTE_LEAF_VERSION = 1;
	constexpr uint32_t RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES = 64;
	constexpr uint32_t RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES = 24;
	constexpr uint32_t RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES =
			(RSS_PAGE_SIZE - RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES) / RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES;
	static_assert(RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES +
				  RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES * RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES == RSS_PAGE_SIZE);

	struct MetadataPageRoute {
		MetadataPageId page_id{};
		PhysicalPageRef physical_reference{};

		constexpr bool operator==(const MetadataPageRoute &) const = default;
	};

	struct MetadataPageRouteLeafPage {
		MetadataPageId page_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		std::vector<MetadataPageRoute> routes;
	};

	class MetadataPageRouteLeafCodec {
	public:
		using Bytes = std::array<std::byte, RSS_PAGE_SIZE>;

		[[nodiscard]] static Bytes encode(const MetadataPageRouteLeafPage &page);
		[[nodiscard]] static MetadataPageRouteLeafPage decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
