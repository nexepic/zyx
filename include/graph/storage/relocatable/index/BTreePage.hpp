/**
 * @file BTreePage.hpp
 * @brief Explicit B+Tree page-header and variable-entry codecs for relocatable.
 *
 * These records live in a checksummed `POK_INDEX` SlottedPage. The slotted-page
 * CRC protects the aggregate page image; these codecs validate record identity,
 * lengths, enum values, and reserved fields before an index reader interprets
 * a record payload.
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
#include <vector>

#include "graph/storage/relocatable/allocation/SlottedPage.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_BTREE_PAGE_HEADER_MAGIC = 0x4842595AU; // Little-endian bytes: ZYBH
	constexpr uint32_t RSS_BTREE_ENTRY_MAGIC = 0x4542595AU; // Little-endian bytes: ZYBE
	// Version 3 adds an explicit leaf-value layout. Version 2 always encoded a
	// value varint, even for a key-only tree, and is intentionally unsupported.
	constexpr uint16_t RSS_BTREE_PAGE_VERSION = 3;
	constexpr uint16_t RSS_BTREE_PAGE_HEADER_BYTES = 72;
	constexpr uint16_t RSS_BTREE_ENTRY_VERSION = 1;
	constexpr uint16_t RSS_BTREE_ENTRY_HEADER_BYTES = 32;
	constexpr uint32_t RSS_BTREE_PACKED_MAGIC = 0x5042595AU; // Little-endian bytes: ZYBP
	constexpr uint16_t RSS_BTREE_PACKED_VERSION = 1;
	constexpr uint16_t RSS_BTREE_PACKED_HEADER_BYTES = 32;
	// Restart points bound the amount of prefix-compressed data decoded for one
	// binary-search probe. The value is a codec invariant, not a page-capacity
	// guess, so changing page or key limits does not change correctness.
	constexpr uint16_t RSS_BTREE_PACKED_RESTART_INTERVAL = 8;

	// Prefixes avoid Windows macro collisions.
	enum class BTreeNodeKind : uint16_t {
		BPN_LEAF = 1,
		BPN_INTERNAL = 2,
	};

	// Prefixes avoid Windows macro collisions. Internal entries always store a
	// right-child PageId; only leaf pages can omit their value bytes.
	enum class BTreeLeafValueLayout : uint16_t {
		BLVL_U64 = 0,
		BLVL_EMPTY = 1,
	};

	struct BTreePageHeader {
		uint64_t index_id = 0;
		PageId page_id{};
		Generation generation{};
		BTreeNodeKind kind = BTreeNodeKind::BPN_LEAF;
		uint16_t level = 0;
		PageId left_sibling{};
		PageId right_sibling{};
		// Internal pages store their child before the first separator here. It is
		// invalid for leaves, which prevents interpreting a leaf payload as a child.
		PageId leftmost_child{};
		uint32_t entry_count = 0;
		BTreeLeafValueLayout leaf_value_layout = BTreeLeafValueLayout::BLVL_U64;

		constexpr bool operator==(const BTreePageHeader &) const = default;
	};

	struct BTreePageEntry {
		std::vector<std::byte> key;
		// Leaf pages interpret this as an index payload; internal pages interpret
		// it as the PageId of the child to the right of the separator.
		uint64_t value_or_child = 0;

		constexpr bool operator==(const BTreePageEntry &) const = default;
	};

	class BTreePageHeaderCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_BTREE_PAGE_HEADER_BYTES>;

		[[nodiscard]] static Bytes encode(const BTreePageHeader &header);
		[[nodiscard]] static BTreePageHeader decode(std::span<const std::byte> bytes);
	};

	class BTreePageEntryCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const BTreePageEntry &entry);
		[[nodiscard]] static BTreePageEntry decode(std::span<const std::byte> bytes);
	};

	// Packed page payload codec. Entries are prefix-compressed between restart
	// points. Leaf pages optionally omit values entirely, while a lookup still
	// probes raw bytes without first materializing the complete page.
	class PackedBTreePageCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte>
		encode(BTreeNodeKind kind, std::span<const BTreePageEntry> entries,
			   BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64);
		[[nodiscard]] static std::vector<BTreePageEntry>
		decode(BTreeNodeKind kind, std::span<const std::byte> bytes,
			   BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64);
		// Checks only the fixed payload envelope and restart table. Page-local point
		// lookups use this before probing one restart group; full entry validation
		// remains the responsibility of decode() and integrity scans.
		[[nodiscard]] static uint32_t entryCount(std::span<const std::byte> bytes);
		[[nodiscard]] static std::optional<BTreePageEntry>
		find(BTreeNodeKind kind, std::span<const std::byte> bytes, std::span<const std::byte> key,
			 BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64);
	};

	// Owns one `POK_INDEX` slotted page. Slot zero is the immutable page header;
	// slot one is the complete packed B+Tree payload.
	class BTreePage final {
	public:
		[[nodiscard]] static BTreePage create(const PageOwnerHeader &owner, const BTreePageHeader &header);
		// Initial and bulk index construction already has an ordered complete page.
		// Encode that payload once instead of repeatedly rebuilding the packed slot.
		[[nodiscard]] static BTreePage createWithEntries(const PageOwnerHeader &owner, BTreePageHeader header,
														 std::span<const BTreePageEntry> entries);
		[[nodiscard]] static BTreePage open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);
		BTreePage(const BTreePage &) = delete;
		BTreePage &operator=(const BTreePage &) = delete;
		BTreePage(BTreePage &&other) noexcept;
		BTreePage &operator=(BTreePage &&other) noexcept;

		[[nodiscard]] BTreePageHeader header() const;
		[[nodiscard]] std::vector<BTreePageEntry> entries() const;
		[[nodiscard]] std::optional<BTreePageEntry> find(std::span<const std::byte> key) const;
		// Rebuilds the typed page under a new physical-page owner while retaining
		// the stable Page ID, tree links, and entries. B+Tree slot IDs are page-
		// local implementation details, so rebuilding also safely compacts holes.
		[[nodiscard]] BTreePage rebindOwner(const PageOwnerHeader &owner) const;
		void insert(BTreePageEntry entry);
		void erase(std::span<const std::byte> key);
		[[nodiscard]] std::vector<std::byte> serialize() const;
		[[nodiscard]] const SlottedPage &slottedPage() const;

	private:
		explicit BTreePage(SlottedPage page);
		[[nodiscard]] std::vector<BTreePageEntry> packedEntries() const;
		[[nodiscard]] SlottedSlotReference validatePackedEnvelope() const;
		void writePackedEntries(std::span<const BTreePageEntry> entries);
		void validateLayout() const;
		void writeHeader(BTreePageHeader header);

		SlottedPage page_;
	};

} // namespace graph::storage
