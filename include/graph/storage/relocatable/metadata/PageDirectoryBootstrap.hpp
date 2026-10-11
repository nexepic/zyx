/**
 * @file PageDirectoryBootstrap.hpp
 * @brief Physical bootstrap header for independently typed page directories.
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

	constexpr uint32_t RSS_PAGE_DIRECTORY_BOOTSTRAP_MAGIC = 0x4258595AU; // Little-endian bytes: ZYXB
	constexpr uint16_t RSS_PAGE_DIRECTORY_BOOTSTRAP_VERSION = 2;
	constexpr uint32_t RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES = 160;
	constexpr size_t RSS_METADATA_DIRECTORY_ROOT_COUNT = 12U;

	// The order is stable on disk and covers every metadata directory whose
	// root used to be addressed directly by the Superblock.
	enum class MetadataDirectoryRootOrdinal : uint8_t {
		MDRO_CATALOG = 0,
		MDRO_ALLOCATOR,
		MDRO_EXTENT,
		MDRO_NODE_ENTITY,
		MDRO_EDGE_ENTITY,
		MDRO_ENTITY_PROPERTY,
		MDRO_INDEX_ROOT,
		MDRO_VECTOR_LOCATION,
		MDRO_VECTOR_MANIFEST,
		MDRO_LARGE_VALUE_MANIFEST,
		MDRO_STATE,
		MDRO_RETIRE_QUEUE,
	};

	// This header is the one physically addressed metadata bootstrap object. Its
	// two branches intentionally retain physical references: resolving either
	// branch through a logical route would reintroduce a bootstrap cycle.
	struct PageDirectoryBootstrap {
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		PhysicalPageRef data_page_directory_root{};
		PhysicalPageRef metadata_page_directory_root{};
		std::array<MetadataPageId, RSS_METADATA_DIRECTORY_ROOT_COUNT> metadata_directory_roots{};

		constexpr bool operator==(const PageDirectoryBootstrap &) const = default;
	};

	class PageDirectoryBootstrapCodec {
	public:
		using Bytes = std::array<std::byte, RSS_PAGE_SIZE>;

		[[nodiscard]] static Bytes encode(const PageDirectoryBootstrap &bootstrap);
		[[nodiscard]] static PageDirectoryBootstrap decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
