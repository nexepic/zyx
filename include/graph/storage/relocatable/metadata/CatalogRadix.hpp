/**
 * @file CatalogRadix.hpp
 * @brief Fixed-width Catalog radix root and internal metadata pages.
 *
 * Catalog leaves persist mappings. These pages route a Segment ID to one leaf
 * through physical metadata references, avoiding both linked lists and a
 * recursive Catalog lookup while the Catalog itself is being opened.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_CATALOG_RADIX_PAGE_MAGIC = 0x5258595AU; // Little-endian bytes: ZYXR
	constexpr uint16_t RSS_CATALOG_RADIX_PAGE_VERSION = 4;
	// Allocator radix pages persist one bit per child indicating that the
	// corresponding subtree still contains at least one reusable physical slot.
	// Keeping the summary in the common radix envelope lets the allocator skip a
	// full subtree without adding another bootstrap root. Non-allocator
	// directories must persist these bytes as zero.
	constexpr uint32_t RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BYTES = 32;
	constexpr uint32_t RSS_CATALOG_RADIX_HEADER_BYTES = 64 + RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BYTES;
	constexpr uint32_t RSS_CATALOG_RADIX_CHILD_BYTES = 16;
	constexpr uint32_t RSS_METADATA_DIRECTORY_TYPE_MASK = 0x000000FFU;
	constexpr uint32_t RSS_CATALOG_RADIX_FANOUT =
			(RSS_PAGE_SIZE - RSS_CATALOG_RADIX_HEADER_BYTES) / RSS_CATALOG_RADIX_CHILD_BYTES;
	constexpr uint32_t RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BITS = RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BYTES * 8U;
	static_assert(RSS_CATALOG_RADIX_FANOUT <= RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BITS);
	constexpr uint16_t RSS_CATALOG_RADIX_MAX_LEVEL = 8;
	static_assert(RSS_CATALOG_RADIX_HEADER_BYTES + RSS_CATALOG_RADIX_FANOUT * RSS_CATALOG_RADIX_CHILD_BYTES ==
				  RSS_PAGE_SIZE);

	// All enumerators have a type prefix to stay safe in Windows builds.
	enum class CatalogRadixPageKind : uint16_t {
		CRPK_ROOT = 1,
		CRPK_INTERNAL = 2,
	};

	// The physical radix layout is shared by bootstrap metadata directories.
	// The type is persisted so one directory can never be decoded as another.
	enum class MetadataDirectoryType : uint8_t {
		MDT_CATALOG = 1,
		MDT_ALLOCATOR = 2,
		MDT_PAGE = 3,
		MDT_EXTENT = 4,
		MDT_NODE_ENTITY = 5,
		MDT_EDGE_ENTITY = 6,
		MDT_INDEX_ROOT = 8,
		MDT_VECTOR_LOCATION = 9,
		MDT_VECTOR_MANIFEST = 10,
		MDT_STATE = 11,
		MDT_RETIRE_QUEUE = 12,
		MDT_ENTITY_PROPERTY = 13,
		MDT_LARGE_VALUE_MANIFEST = 14,
	};

	struct CatalogRadixPage {
		CatalogRadixPageKind kind = CatalogRadixPageKind::CRPK_INTERNAL;
		MetadataDirectoryType directory_type = MetadataDirectoryType::MDT_CATALOG;
		uint16_t level = 1;
		PageId page_id{};
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		std::array<std::byte, RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BYTES> allocator_free_children{};
		std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT> children{};
	};

	class CatalogRadixPageCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_PAGE_SIZE> encode(const CatalogRadixPage &page);
		[[nodiscard]] static CatalogRadixPage decode(std::span<const std::byte> bytes);

		// Segment IDs first map to a 72-entry Catalog-leaf ordinal. Level zero
		// denotes that ordinal's leaf digit; a root at level eight covers every
		// positive 64-bit Segment ID with this fixed-fan-out representation.
		[[nodiscard]] static uint32_t childIndex(SegmentId segmentId, uint16_t level);
		[[nodiscard]] static uint32_t childIndexForLeafOrdinal(uint64_t leafOrdinal, uint16_t level);
		// Returns no value when a valid sparse Catalog route has no child or the
		// root has not grown far enough to cover the requested logical ID.
		[[nodiscard]] static std::optional<PhysicalPageRef> findChildFor(const CatalogRadixPage &page,
																		 SegmentId segmentId);
		[[nodiscard]] static const PhysicalPageRef &childFor(const CatalogRadixPage &page, SegmentId segmentId);
		// Directory implementations route Page and Extent leaf ordinals through
		// the same sparse radix format without pretending those IDs are Segment IDs.
		[[nodiscard]] static std::optional<PhysicalPageRef> findChildForLeafOrdinal(const CatalogRadixPage &page,
																					uint64_t leafOrdinal);
		[[nodiscard]] static const PhysicalPageRef &childForLeafOrdinal(const CatalogRadixPage &page,
																		uint64_t leafOrdinal);
	};

	// These aliases express the shared physical layout without changing the
	// established Catalog-facing API while other directories are introduced.
	using MetadataRadixPage = CatalogRadixPage;
	using MetadataRadixPageCodec = CatalogRadixPageCodec;

} // namespace graph::storage
