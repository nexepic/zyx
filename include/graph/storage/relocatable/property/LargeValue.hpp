/**
 * @file LargeValue.hpp
 * @brief Explicit immutable 4 KiB-page codecs for generic external values.
 *
 * A LargeValue is intentionally domain-neutral: source code, JSON, images,
 * and future binary payloads all use the same logical object, manifest, and
 * chunk records. Physical placement is carried only by Page Directory entries;
 * the logical reference below never embeds a SegmentId or PhysicalSlotId.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_LARGE_VALUE_REF_MAGIC = 0x5256595AU; // Little-endian bytes: ZYVR
	constexpr uint16_t RSS_LARGE_VALUE_REF_VERSION = 1;
	constexpr uint16_t RSS_LARGE_VALUE_REF_HEADER_BYTES = 96;
	constexpr uint32_t RSS_LARGE_VALUE_MANIFEST_MAGIC = 0x564C595AU; // Little-endian bytes: ZYLV
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_VERSION = 2;
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES = 256;
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES = 40;
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES = 32;
	constexpr uint32_t RSS_LARGE_VALUE_DATA_PAGE_MAGIC = 0x4456595AU; // Little-endian bytes: ZYVD
	constexpr uint16_t RSS_LARGE_VALUE_DATA_PAGE_VERSION = 1;
	constexpr uint16_t RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES = 80;
	constexpr size_t RSS_LARGE_VALUE_CONTENT_HASH_BYTES = 32U;
	// This logical B+Tree is owned exclusively by LargeValueStore. Its key
	// contains the complete immutable content identity, never a truncated hash.
	constexpr uint64_t RSS_LARGE_VALUE_CONTENT_INDEX_ID = std::numeric_limits<uint64_t>::max();
	constexpr size_t RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES = 64U;
	// Data pages from many immutable values share this typed pool namespace. The
	// Page owner retains the value identity, so pooling reduces tail waste without
	// weakening per-value validation or changing a LargeValue reference.
	constexpr uint64_t RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID = 1U;
	// Manifest frames retain their actual ValueId in PageOwnerHeader, while the
	// Catalog owner names one shared placement namespace. Keeping the namespace
	// distinct from the data-pool namespace makes a cross-kind routing error fail
	// before any frame payload is decoded.
	constexpr uint64_t RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID = 2U;
	// This is a public format-policy bound, not a capacity-derived entry count.
	// It prevents metadata from turning an otherwise bounded Property record into
	// an unbounded string container.
	constexpr size_t RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES = 128U;
	constexpr uint32_t RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES =
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES;
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES) /
			RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES;
	constexpr uint16_t RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES) /
			RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES;

	static_assert(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES >= 128U + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES);
	static_assert(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES != 0U);
	static_assert(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS != 0U);
	static_assert(RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN != 0U);
	static_assert(RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID != 0U);
	static_assert(RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID != 0U);
	static_assert(RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID != RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	static_assert(RSS_LARGE_VALUE_CONTENT_INDEX_ID != 0U);
	constexpr uint8_t RSS_LARGE_VALUE_DEFAULT_MIN_SAVINGS_PERCENT = 5U;
	constexpr int RSS_LARGE_VALUE_DEFAULT_ZSTD_LEVEL = 3;
	constexpr uint32_t RSS_LARGE_VALUE_EXTENT_16K_BYTES = 16U * 1024U;
	constexpr uint32_t RSS_LARGE_VALUE_EXTENT_32K_BYTES = 32U * 1024U;
	constexpr uint32_t RSS_LARGE_VALUE_EXTENT_64K_BYTES = 64U * 1024U;
	constexpr uint32_t RSS_LARGE_VALUE_EXTENT_128K_BYTES = 128U * 1024U;

	// Prefixes make persisted enum identifiers safe when Windows headers are in
	// the same translation unit.
	enum class LargeValueKind : uint16_t {
		LVK_BYTES = 1,
		LVK_UTF8_TEXT = 2,
	};

	// A manifest may contain chunks that use different codecs. Writers currently
	// emit only a supported codec; readers reject any unknown stored codec before
	// exposing bytes to a caller.
	enum class LargeValueCodec : uint16_t {
		LVC_RAW = 1,
		LVC_LZ4 = 2,
		LVC_ZSTD = 3,
		LVC_MIXED = 4,
	};

	// Root nodes are named by LargeValueManifestDirectory. Leaf nodes store
	// chunks, while internal nodes store ordered child ranges. All node PageIds
	// remain stable across physical compaction.
	enum class LargeValueManifestNodeKind : uint32_t {
		LVMNK_ROOT = 1,
		LVMNK_INTERNAL = 2,
		LVMNK_LEAF = 3,
	};

	// Logical extents tune chunk locality and compression working-set size. They
	// never alter the fixed physical Segment size or the 4 KiB page unit.
	enum class LargeValueExtentClass : uint16_t {
		LVEC_16K = 1,
		LVEC_32K = 2,
		LVEC_64K = 3,
		LVEC_128K = 4,
	};

	// Compression is selected independently for each immutable chunk. A chunk
	// that does not meet the configured savings threshold is stored as RAW when
	// fallback is enabled, keeping placement bounded and range reads local.
	struct LargeValueCompressionOptions {
		LargeValueCodec codec = LargeValueCodec::LVC_RAW;
		uint8_t minimum_savings_percent = RSS_LARGE_VALUE_DEFAULT_MIN_SAVINGS_PERCENT;
		int zstd_level = RSS_LARGE_VALUE_DEFAULT_ZSTD_LEVEL;
		bool allow_raw_fallback = true;
		// Transient requested logical cut points; manifests already store explicit
		// contiguous offsets/lengths. Never serialized as a new codec or root kind.
		std::vector<uint64_t> logical_chunk_ends{};

		constexpr bool operator==(const LargeValueCompressionOptions &) const = default;
	};

	struct LargeValueRef {
		LargeValueId value_id{};
		// Logical immutable generation. Physical frame generation belongs solely
		// to PageOwnerHeader and PageDirectory so compaction can move this value
		// without rewriting its reference or its manifest contents.
		Generation generation{};
		uint64_t logical_bytes = 0U;
		std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> content_hash{};
		LargeValueKind kind = LargeValueKind::LVK_BYTES;
		LargeValueCodec codec = LargeValueCodec::LVC_RAW;
		std::string media_type;

		constexpr bool operator==(const LargeValueRef &) const = default;
	};

	// Chunks are immutable. The page-ID range is assigned in one transaction and
	// remains logical, so compaction rewrites only Page Directory locations.
	struct LargeValueChunk {
		uint32_t ordinal = 0U;
		LargeValueCodec codec = LargeValueCodec::LVC_RAW;
		uint64_t uncompressed_offset = 0U;
		uint32_t uncompressed_bytes = 0U;
		uint32_t stored_bytes = 0U;
		uint32_t uncompressed_crc = 0U;
		uint32_t stored_crc = 0U;
		PageId first_data_page_id{};
		uint16_t data_page_count = 0U;

		constexpr bool operator==(const LargeValueChunk &) const = default;
	};

	struct LargeValueManifestChild {
		PageId page_id{};
		uint32_t first_chunk_ordinal = 0U;
		uint32_t chunk_count = 0U;
		uint64_t uncompressed_offset = 0U;
		uint64_t logical_bytes = 0U;

		constexpr bool operator==(const LargeValueManifestChild &) const = default;
	};

	struct LargeValueManifest {
		LargeValueId value_id{};
		PageId page_id{};
		// Logical immutable generation; this is intentionally independent from
		// the physical Page Directory location generation.
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		uint64_t logical_bytes = 0U;
		std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> content_hash{};
		LargeValueKind kind = LargeValueKind::LVK_BYTES;
		LargeValueCodec codec = LargeValueCodec::LVC_RAW;
		std::string media_type;
		LargeValueManifestNodeKind node_kind = LargeValueManifestNodeKind::LVMNK_ROOT;
		uint16_t node_level = 0U;
		uint32_t first_chunk_ordinal = 0U;
		uint64_t first_uncompressed_offset = 0U;
		// Root nodes leave this zero because their span is `logical_bytes`.
		// Internal and leaf nodes store their exact logical range length.
		uint64_t node_logical_bytes = 0U;
		std::vector<LargeValueChunk> chunks;
		std::vector<LargeValueManifestChild> children;

		[[nodiscard]] LargeValueRef reference() const;
		[[nodiscard]] constexpr bool isRoot() const {
			return node_kind == LargeValueManifestNodeKind::LVMNK_ROOT;
		}
		[[nodiscard]] constexpr bool isLeaf() const {
			return node_kind == LargeValueManifestNodeKind::LVMNK_LEAF ||
				   (node_kind == LargeValueManifestNodeKind::LVMNK_ROOT && node_level == 0U);
		}
		constexpr bool operator==(const LargeValueManifest &) const = default;
	};

	struct LargeValueDataPage {
		LargeValueId value_id{};
		PageId page_id{};
		// Logical immutable generation shared with the owning LargeValue manifest.
		// It is not the physical frame generation used by PageOwnerHeader.
		Generation generation{};
		uint32_t chunk_ordinal = 0U;
		uint16_t page_ordinal = 0U;
		uint16_t page_count = 0U;
		uint32_t stored_chunk_bytes = 0U;
		uint32_t stored_offset = 0U;
		std::vector<std::byte> payload;

		constexpr bool operator==(const LargeValueDataPage &) const = default;
	};

	class LargeValueRefCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const LargeValueRef &reference);
		[[nodiscard]] static LargeValueRef decode(std::span<const std::byte> bytes);
	};

	class LargeValueManifestPageCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const LargeValueManifest &manifest);
		[[nodiscard]] static LargeValueManifest decode(std::span<const std::byte> bytes,
				const std::function<void(size_t)> &bufferAdmission = {});
	};

	class LargeValueDataPageCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const LargeValueDataPage &page);
		[[nodiscard]] static LargeValueDataPage decode(std::span<const std::byte> bytes,
				const std::function<void(size_t)> &bufferAdmission = {});
	};

	class LargeValueManifestPage final {
	public:
		[[nodiscard]] static LargeValueManifest decodeManifest(const PageOwnerHeader &owner,
				std::span<const std::byte> bytes,
				const std::function<void(size_t)> &bufferAdmission = {});
		[[nodiscard]] static LargeValueManifestPage create(const PageOwnerHeader &owner,
														   const LargeValueManifest &manifest);
		[[nodiscard]] static LargeValueManifestPage open(const PageOwnerHeader &owner,
														 std::span<const std::byte> bytes);

		[[nodiscard]] const LargeValueManifest &manifest() const { return manifest_; }
		[[nodiscard]] const std::vector<std::byte> &serialize() const { return bytes_; }
		[[nodiscard]] LargeValueManifestPage rebindOwner(const PageOwnerHeader &owner) const;

	private:
		LargeValueManifestPage(std::vector<std::byte> bytes, LargeValueManifest manifest);

		std::vector<std::byte> bytes_;
		LargeValueManifest manifest_;
	};

	class LargeValueDataFrame final {
	public:
		[[nodiscard]] static LargeValueDataPage decodePage(const PageOwnerHeader &owner,
				std::span<const std::byte> bytes,
				const std::function<void(size_t)> &bufferAdmission = {});
		[[nodiscard]] static LargeValueDataFrame create(const PageOwnerHeader &owner, const LargeValueDataPage &page);
		[[nodiscard]] static LargeValueDataFrame open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);

		[[nodiscard]] const LargeValueDataPage &page() const { return page_; }
		[[nodiscard]] const std::vector<std::byte> &serialize() const { return bytes_; }
		[[nodiscard]] LargeValueDataFrame rebindOwner(const PageOwnerHeader &owner) const;

	private:
		LargeValueDataFrame(std::vector<std::byte> bytes, LargeValueDataPage page);

		std::vector<std::byte> bytes_;
		LargeValueDataPage page_;
	};

} // namespace graph::storage
