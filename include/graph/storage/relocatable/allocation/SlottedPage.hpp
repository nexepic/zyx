/**
 * @file SlottedPage.hpp
 * @brief Checked variable-record page codec for the experimental relocatable format.
 *
 * A SlottedPage occupies the payload area of one shared-pool frame.  Its
 * stable references contain a Page ID, slot ID, and slot generation; payload
 * offsets are intentionally page-local implementation details.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_SLOTTED_PAGE_MAGIC = 0x4C53595AU; // Little-endian bytes: ZYSL
	constexpr uint16_t RSS_SLOTTED_PAGE_VERSION = 1;
	constexpr uint16_t RSS_SLOTTED_PAGE_HEADER_BYTES = 64;
	constexpr uint16_t RSS_SLOTTED_PAGE_SLOT_BYTES = 16;
	constexpr uint16_t RSS_SLOTTED_PAGE_NO_SLOT = 0xFFFFU;
	constexpr uint16_t RSS_SLOTTED_PAGE_KNOWN_FLAGS = 0;
	constexpr uint16_t RSS_SLOTTED_SLOT_FLAG_LIVE = 1U;
	constexpr uint16_t RSS_SLOTTED_SLOT_KNOWN_FLAGS = RSS_SLOTTED_SLOT_FLAG_LIVE;

	struct SlottedSlotReference {
		PageId page_id{};
		uint16_t slot_id = RSS_SLOTTED_PAGE_NO_SLOT;
		uint32_t generation = 0;

		[[nodiscard]] constexpr bool isValid() const {
			return page_id.isValid() && slot_id != RSS_SLOTTED_PAGE_NO_SLOT && generation != 0U;
		}
		constexpr auto operator<=>(const SlottedSlotReference &) const = default;
	};

	struct SlottedPageStats {
		uint16_t slot_count = 0;
		uint16_t live_slot_count = 0;
		// A free slot with an exhausted generation cannot be safely reused. Keep
		// this separately from all dead slots so placement accounts for the slot
		// directory cost only when a new entry is really necessary.
		uint16_t reusable_free_slot_count = 0;
		uint32_t live_bytes = 0;
		uint32_t reclaimable_bytes = 0;
		uint32_t contiguous_free_bytes = 0;
	};

	// Read-only counterpart to SlottedPage. It borrows a fully pinned shared-pool
	// payload after authenticating the page and a stable slot reference. COW
	// writers keep using the owning SlottedPage representation below.
	class SlottedPageReadView final {
	public:
		[[nodiscard]] static SlottedPageReadView open(const PageOwnerHeader &owner,
																	 std::span<const std::byte> bytes);

		[[nodiscard]] PageId pageId() const { return page_id_; }
		[[nodiscard]] Generation pageGeneration() const { return page_generation_; }
		[[nodiscard]] uint64_t ownerId() const { return owner_id_; }
		[[nodiscard]] PageOwnerKind ownerKind() const { return owner_kind_; }
		[[nodiscard]] LogSequenceNumber ownerCommittedLsn() const { return owner_committed_lsn_; }
		[[nodiscard]] SlottedPageStats stats() const;
		[[nodiscard]] std::span<const std::byte> read(const SlottedSlotReference &reference) const;

	private:
		struct Slot {
			uint16_t offset = 0;
			uint16_t capacity = 0;
			uint16_t value_bytes = 0;
			uint16_t flags = 0;
			uint32_t generation = 0;

			[[nodiscard]] bool isLive() const { return flags == RSS_SLOTTED_SLOT_FLAG_LIVE; }
		};

		SlottedPageReadView(std::span<const std::byte> bytes, PageId pageId, Generation pageGeneration,
								uint64_t ownerId, PageOwnerKind ownerKind, LogSequenceNumber ownerCommittedLsn,
								uint16_t slotCount, uint16_t liveSlotCount, uint16_t freeStart, uint16_t freeEnd,
								uint32_t liveBytes, uint32_t reclaimableBytes) :
				bytes_(bytes), page_id_(pageId), page_generation_(pageGeneration), owner_id_(ownerId),
				owner_kind_(ownerKind), owner_committed_lsn_(ownerCommittedLsn), slot_count_(slotCount),
				live_slot_count_(liveSlotCount), free_start_(freeStart), free_end_(freeEnd),
				live_bytes_(liveBytes), reclaimable_bytes_(reclaimableBytes) {}

		[[nodiscard]] Slot readSlot(uint16_t slotId) const;
		[[nodiscard]] size_t slotOffset(uint16_t slotId) const;
		[[nodiscard]] uint16_t reusableFreeSlotCount() const;

		std::span<const std::byte> bytes_;
		PageId page_id_{};
		Generation page_generation_{};
		uint64_t owner_id_ = 0U;
		PageOwnerKind owner_kind_ = PageOwnerKind::POK_NONE;
		LogSequenceNumber owner_committed_lsn_{};
		uint16_t slot_count_ = 0U;
		uint16_t live_slot_count_ = 0U;
		uint16_t free_start_ = 0U;
		uint16_t free_end_ = 0U;
		uint32_t live_bytes_ = 0U;
		uint32_t reclaimable_bytes_ = 0U;
	};

	// The object serializes its own page-local mutations. Persisting its returned
	// bytes and atomically publishing the matching Page Directory entry remains
	// the responsibility of a higher-level COW store.
	class SlottedPage final {
	public:
		[[nodiscard]] static constexpr bool supportsOwnerKind(PageOwnerKind kind) {
			switch (kind) {
				case PageOwnerKind::POK_PROPERTY:
				case PageOwnerKind::POK_INDEX:
				case PageOwnerKind::POK_STATE:
				case PageOwnerKind::POK_VECTOR_ADJACENCY:
				case PageOwnerKind::POK_ENTITY_LABEL:
					return true;
				default:
					return false;
			}
		}

		[[nodiscard]] static SlottedPage create(PageId pageId, Generation pageGeneration);
		[[nodiscard]] static SlottedPage create(const PageOwnerHeader &owner);
		[[nodiscard]] static SlottedPage open(std::span<const std::byte> bytes);
		[[nodiscard]] static SlottedPage open(const PageOwnerHeader &owner, std::span<const std::byte> bytes);

		SlottedPage(const SlottedPage &) = delete;
		SlottedPage &operator=(const SlottedPage &) = delete;
		SlottedPage(SlottedPage &&other) noexcept;
		SlottedPage &operator=(SlottedPage &&other) noexcept;

		[[nodiscard]] PageId pageId() const;
		[[nodiscard]] Generation pageGeneration() const;
		[[nodiscard]] uint64_t ownerId() const;
		[[nodiscard]] PageOwnerKind ownerKind() const;
		[[nodiscard]] LogSequenceNumber ownerCommittedLsn() const;
		[[nodiscard]] SlottedPageStats stats() const;
		// Produces an independent mutable image while preserving the authenticated
		// owner header. Batch planners use it to reuse one pinned source page.
		[[nodiscard]] SlottedPage clone() const;
		[[nodiscard]] std::vector<std::byte> serialize() const;
		// Enumerates stable references for all currently live records. The caller
		// still validates each decoded domain record before interpreting it.
		[[nodiscard]] std::vector<SlottedSlotReference> liveSlots() const;
		// Rebinds the page to a replacement shared-pool owner without changing its
		// stable Page ID or any slot reference.
		void rebindOwner(const PageOwnerHeader &owner);

		[[nodiscard]] SlottedSlotReference insert(std::span<const std::byte> value);
		[[nodiscard]] std::vector<std::byte> read(const SlottedSlotReference &reference) const;
		void overwrite(const SlottedSlotReference &reference, std::span<const std::byte> value);
		void erase(const SlottedSlotReference &reference);
		void compact();

	private:
		struct Header {
			PageId page_id{};
			Generation page_generation{};
			uint64_t owner_id = 0;
			PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
			LogSequenceNumber owner_committed_lsn{};
			uint16_t slot_count = 0;
			uint16_t live_slot_count = 0;
			uint16_t free_slot_hint = RSS_SLOTTED_PAGE_NO_SLOT;
			uint16_t free_start = RSS_SLOTTED_PAGE_HEADER_BYTES;
			uint16_t free_end = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
			uint32_t live_bytes = 0;
			uint32_t reclaimable_bytes = 0;
		};

		struct Slot {
			uint16_t offset = 0;
			uint16_t capacity = 0;
			uint16_t value_bytes = 0;
			uint16_t flags = 0;
			uint32_t generation = 0;

			[[nodiscard]] bool isLive() const { return flags == RSS_SLOTTED_SLOT_FLAG_LIVE; }
		};

		explicit SlottedPage(std::vector<std::byte> bytes, Header header);

		[[nodiscard]] SlottedSlotReference insertLocked(std::span<const std::byte> value);
		[[nodiscard]] std::vector<std::byte> readLocked(const SlottedSlotReference &reference) const;
		void overwriteLocked(const SlottedSlotReference &reference, std::span<const std::byte> value);
		void eraseLocked(const SlottedSlotReference &reference);
		void compactLocked();
		void compactExcludingLocked(uint16_t excludedSlot);
		[[nodiscard]] uint16_t firstFreeSlotLocked() const;
		[[nodiscard]] uint16_t firstReusableFreeSlotLocked() const;
		[[nodiscard]] uint16_t reusableFreeSlotCountLocked() const;
		[[nodiscard]] Slot readSlotLocked(uint16_t slotId) const;
		void writeSlotLocked(uint16_t slotId, const Slot &slot);
		[[nodiscard]] size_t slotOffset(uint16_t slotId) const;
		void validateReferenceLocked(const SlottedSlotReference &reference) const;
		void refreshHeaderLocked();
		void refreshDerivedFieldsLocked();
		[[nodiscard]] uint32_t contiguousFreeLocked() const;

		mutable std::mutex latch_;
		std::vector<std::byte> bytes_;
		Header header_;
	};

} // namespace graph::storage
