/**
 * @file SlottedPage.cpp
 * @brief Checked variable-record page implementation for the relocatable format.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/SlottedPage.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t PAGE_MAGIC_OFFSET = 0;
		constexpr size_t PAGE_VERSION_OFFSET = 4;
		constexpr size_t PAGE_HEADER_BYTES_OFFSET = 6;
		constexpr size_t PAGE_ID_OFFSET = 8;
		constexpr size_t PAGE_GENERATION_OFFSET = 16;
		constexpr size_t PAGE_SLOT_COUNT_OFFSET = 24;
		constexpr size_t PAGE_LIVE_SLOT_COUNT_OFFSET = 26;
		constexpr size_t PAGE_FREE_SLOT_HINT_OFFSET = 28;
		constexpr size_t PAGE_RESERVED0_OFFSET = 30;
		constexpr size_t PAGE_FREE_START_OFFSET = 32;
		constexpr size_t PAGE_FREE_END_OFFSET = 34;
		constexpr size_t PAGE_RESERVED1_OFFSET = 36;
		constexpr size_t PAGE_LIVE_BYTES_OFFSET = 40;
		constexpr size_t PAGE_RECLAIMABLE_BYTES_OFFSET = 44;
		constexpr size_t PAGE_FLAGS_OFFSET = 48;
		constexpr size_t PAGE_CRC_OFFSET = 52;
		constexpr size_t PAGE_RESERVED2_OFFSET = 56;

		constexpr size_t SLOT_OFFSET_OFFSET = 0;
		constexpr size_t SLOT_CAPACITY_OFFSET = 2;
		constexpr size_t SLOT_VALUE_BYTES_OFFSET = 4;
		constexpr size_t SLOT_FLAGS_OFFSET = 6;
		constexpr size_t SLOT_GENERATION_OFFSET = 8;
		constexpr size_t SLOT_RESERVED_OFFSET = 12;

		[[noreturn]] void throwSlottedPageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] uint16_t checkedU16(size_t value, const char *message) {
			if (value > std::numeric_limits<uint16_t>::max()) {
				throwSlottedPageError(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return static_cast<uint16_t>(value);
		}

		void validateSlottedPageOwner(const PageOwnerHeader &owner) {
			const uint8_t invalid = static_cast<uint8_t>(!owner.page_id.isValid()) |
									static_cast<uint8_t>(!owner.generation.isValid()) |
									static_cast<uint8_t>(!owner.committed_lsn.isValid()) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(!SlottedPage::supportsOwnerKind(owner.kind)) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U);
			if (invalid != 0U) {
				throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
									  "relocatable slotted page requires a complete variable-record page owner");
			}
		}

	} // namespace

	SlottedPage::SlottedPage(std::vector<std::byte> bytes, Header header) :
		bytes_(std::move(bytes)), header_(std::move(header)) {}

	SlottedPage::SlottedPage(SlottedPage &&other) noexcept {
		std::scoped_lock lock(other.latch_);
		bytes_ = std::move(other.bytes_);
		header_ = other.header_;
	}

	SlottedPage &SlottedPage::operator=(SlottedPage &&other) noexcept {
		if (this == &other) { // ZYX_COV_EXCL_LINE: callers cannot observe useful self-move state.
			return *this;
		}
		std::scoped_lock lock(latch_, other.latch_);
		bytes_ = std::move(other.bytes_);
		header_ = other.header_;
		return *this;
	}

	SlottedPage SlottedPage::create(PageId pageId, Generation pageGeneration) {
		if (!pageId.isValid() || !pageGeneration.isValid()) {
			throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page requires a valid Page ID and generation");
		}
		Header header;
		header.page_id = pageId;
		header.page_generation = pageGeneration;
		SlottedPage page(std::vector<std::byte>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES), std::move(header));
		page.refreshHeaderLocked();
		return page;
	}

	SlottedPage SlottedPage::create(const PageOwnerHeader &owner) {
		validateSlottedPageOwner(owner);
		SlottedPage page = create(owner.page_id, owner.generation);
		page.header_.owner_id = owner.owner_id;
		page.header_.owner_kind = owner.kind;
		page.header_.owner_committed_lsn = owner.committed_lsn;
		return page;
	}

	SlottedPage SlottedPage::open(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwSlottedPageError(bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																				  : FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page has an invalid byte length");
		}
		std::vector<std::byte> copied(bytes.begin(), bytes.end());
		const uint32_t expectedCrc = LittleEndianCodec::readU32(copied, PAGE_CRC_OFFSET);
		LittleEndianCodec::writeU32(copied, PAGE_CRC_OFFSET, 0);
		if (calculateCrc(copied) != expectedCrc) {
			throwSlottedPageError(FormatError::FERR_CHECKSUM_MISMATCH,
								  "relocatable slotted page checksum does not match");
		}
		LittleEndianCodec::writeU32(copied, PAGE_CRC_OFFSET, expectedCrc);
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(copied, PAGE_MAGIC_OFFSET) != RSS_SLOTTED_PAGE_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(copied, PAGE_VERSION_OFFSET) !=
									 RSS_SLOTTED_PAGE_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(copied, PAGE_HEADER_BYTES_OFFSET) !=
									 RSS_SLOTTED_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(copied, PAGE_RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(copied, PAGE_RESERVED1_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(copied, PAGE_FLAGS_OFFSET) !=
									 RSS_SLOTTED_PAGE_KNOWN_FLAGS) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(copied, PAGE_RESERVED2_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			throwSlottedPageError(FormatError::FERR_RESERVED_BITS, "relocatable slotted page has invalid fixed fields");
		}
		Header header;
		header.page_id = PageId(LittleEndianCodec::readU64(copied, PAGE_ID_OFFSET));
		header.page_generation = Generation(LittleEndianCodec::readU64(copied, PAGE_GENERATION_OFFSET));
		header.slot_count = LittleEndianCodec::readU16(copied, PAGE_SLOT_COUNT_OFFSET);
		header.live_slot_count = LittleEndianCodec::readU16(copied, PAGE_LIVE_SLOT_COUNT_OFFSET);
		header.free_slot_hint = LittleEndianCodec::readU16(copied, PAGE_FREE_SLOT_HINT_OFFSET);
		header.free_start = LittleEndianCodec::readU16(copied, PAGE_FREE_START_OFFSET);
		header.free_end = LittleEndianCodec::readU16(copied, PAGE_FREE_END_OFFSET);
		header.live_bytes = LittleEndianCodec::readU32(copied, PAGE_LIVE_BYTES_OFFSET);
		header.reclaimable_bytes = LittleEndianCodec::readU32(copied, PAGE_RECLAIMABLE_BYTES_OFFSET);
		if (!header.page_id.isValid() || !header.page_generation.isValid() ||
			header.slot_count >
					(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES) / RSS_SLOTTED_PAGE_SLOT_BYTES ||
			header.live_slot_count > header.slot_count ||
			header.free_start != RSS_SLOTTED_PAGE_HEADER_BYTES + header.slot_count * RSS_SLOTTED_PAGE_SLOT_BYTES ||
			header.free_start > header.free_end || header.free_end > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page has invalid layout metadata");
		}
		SlottedPage page(std::move(copied), std::move(header));
		uint32_t actualLiveSlots = 0;
		uint32_t actualLiveBytes = 0;
		uint32_t allocatedBytes = 0;
		uint16_t expectedFreeHint = RSS_SLOTTED_PAGE_NO_SLOT;
		for (uint16_t slotId = 0; slotId < page.header_.slot_count; ++slotId) {
			const Slot slot = page.readSlotLocked(slotId);
			if (slot.generation == 0U || (slot.flags & ~RSS_SLOTTED_SLOT_KNOWN_FLAGS) != 0U ||
				LittleEndianCodec::readU32(page.bytes_, page.slotOffset(slotId) + SLOT_RESERVED_OFFSET) != 0U) {
				throwSlottedPageError(FormatError::FERR_RESERVED_BITS,
									  "relocatable slotted page slot has an invalid generation or reserved field");
			}
			if (!slot.isLive()) {
				if (slot.flags != 0U || slot.offset != 0U || slot.capacity != 0U || slot.value_bytes != 0U) {
					throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
										  "relocatable slotted page free slot retains payload metadata");
				}
				if (expectedFreeHint == RSS_SLOTTED_PAGE_NO_SLOT) {
					expectedFreeHint = slotId;
				}
				continue;
			}
			if (slot.value_bytes > slot.capacity || slot.offset < page.header_.free_end ||
				slot.capacity > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - slot.offset) {
				throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
									  "relocatable slotted page live slot has an invalid payload range");
			}
			for (uint16_t otherSlotId = 0; otherSlotId < slotId; ++otherSlotId) {
				const Slot other = page.readSlotLocked(otherSlotId);
				if (!other.isLive() || slot.capacity == 0U || other.capacity == 0U) {
					continue;
				}
				const uint32_t slotEnd = static_cast<uint32_t>(slot.offset) + slot.capacity;
				const uint32_t otherEnd = static_cast<uint32_t>(other.offset) + other.capacity;
				if (slot.offset < otherEnd && other.offset < slotEnd) {
					throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
										  "relocatable slotted page live slot payload ranges overlap");
				}
			}
			++actualLiveSlots;
			actualLiveBytes += slot.value_bytes;
			allocatedBytes += slot.capacity;
		}
		const uint32_t payloadRegionBytes = page.header_.free_end <= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES
													? RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - page.header_.free_end
													: 0U;
		if (actualLiveSlots != page.header_.live_slot_count || actualLiveBytes != page.header_.live_bytes ||
			allocatedBytes > payloadRegionBytes ||
			page.header_.reclaimable_bytes != payloadRegionBytes - allocatedBytes ||
			page.header_.free_slot_hint != expectedFreeHint) {
			throwSlottedPageError(FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page counters or free-slot hint do not match its slots");
		}
		return page;
	}

	SlottedPage SlottedPage::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		validateSlottedPageOwner(owner);
		SlottedPage page = open(bytes);
		if (page.pageId() != owner.page_id || page.pageGeneration() != owner.generation) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page header disagrees with its shared-pool owner");
		}
		page.header_.owner_id = owner.owner_id;
		page.header_.owner_kind = owner.kind;
		page.header_.owner_committed_lsn = owner.committed_lsn;
		return page;
	}

	PageId SlottedPage::pageId() const {
		std::scoped_lock lock(latch_);
		return header_.page_id;
	}

	Generation SlottedPage::pageGeneration() const {
		std::scoped_lock lock(latch_);
		return header_.page_generation;
	}

	uint64_t SlottedPage::ownerId() const {
		std::scoped_lock lock(latch_);
		return header_.owner_id;
	}

	PageOwnerKind SlottedPage::ownerKind() const {
		std::scoped_lock lock(latch_);
		return header_.owner_kind;
	}

	LogSequenceNumber SlottedPage::ownerCommittedLsn() const {
		std::scoped_lock lock(latch_);
		return header_.owner_committed_lsn;
	}

	SlottedPageStats SlottedPage::stats() const {
		std::scoped_lock lock(latch_);
		return {header_.slot_count, header_.live_slot_count,   reusableFreeSlotCountLocked(),
				header_.live_bytes, header_.reclaimable_bytes, contiguousFreeLocked()};
	}

	SlottedPage SlottedPage::clone() const {
		std::scoped_lock lock(latch_);
		return SlottedPage(bytes_, header_);
	}

	std::vector<std::byte> SlottedPage::serialize() const {
		std::scoped_lock lock(latch_);
		return bytes_;
	}

	std::vector<SlottedSlotReference> SlottedPage::liveSlots() const {
		std::scoped_lock lock(latch_);
		std::vector<SlottedSlotReference> references;
		references.reserve(header_.live_slot_count);
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			const Slot slot = readSlotLocked(slotId);
			if (slot.isLive()) {
				references.push_back({header_.page_id, slotId, slot.generation});
			}
		}
		return references;
	}

	void SlottedPage::rebindOwner(const PageOwnerHeader &owner) {
		validateSlottedPageOwner(owner);
		if (owner.flags != 0U) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page cannot bind a retired shared-pool owner");
		}
		std::scoped_lock lock(latch_);
		if (owner.page_id != header_.page_id) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page cannot change its stable Page ID while rebinding an owner");
		}
		if ((header_.owner_kind != PageOwnerKind::POK_NONE &&
			 (owner.owner_id != header_.owner_id || owner.kind != header_.owner_kind))) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page cannot change its logical owner while rebinding a frame");
		}
		header_.page_generation = owner.generation;
		header_.owner_id = owner.owner_id;
		header_.owner_kind = owner.kind;
		header_.owner_committed_lsn = owner.committed_lsn;
		refreshHeaderLocked();
	}

	SlottedSlotReference SlottedPage::insert(std::span<const std::byte> value) {
		std::scoped_lock lock(latch_);
		return insertLocked(value);
	}

	std::vector<std::byte> SlottedPage::read(const SlottedSlotReference &reference) const {
		std::scoped_lock lock(latch_);
		return readLocked(reference);
	}

	void SlottedPage::overwrite(const SlottedSlotReference &reference, std::span<const std::byte> value) {
		std::scoped_lock lock(latch_);
		overwriteLocked(reference, value);
	}

	void SlottedPage::erase(const SlottedSlotReference &reference) {
		std::scoped_lock lock(latch_);
		eraseLocked(reference);
	}

	void SlottedPage::compact() {
		std::scoped_lock lock(latch_);
		compactLocked();
	}

	SlottedSlotReference SlottedPage::insertLocked(std::span<const std::byte> value) {
		const uint16_t valueBytes =
				checkedU16(value.size(), "relocatable slotted page record exceeds 16-bit slot capacity");
		uint16_t slotId = firstReusableFreeSlotLocked();
		const bool needsSlot = slotId == RSS_SLOTTED_PAGE_NO_SLOT;
		const uint32_t required = static_cast<uint32_t>(valueBytes) + (needsSlot ? RSS_SLOTTED_PAGE_SLOT_BYTES : 0U);
		if (required > contiguousFreeLocked() && required > contiguousFreeLocked() + header_.reclaimable_bytes) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page has insufficient free space");
		}
		if (required > contiguousFreeLocked()) {
			compactLocked();
		}
		Slot slot;
		if (needsSlot) {
			slotId = header_.slot_count;
			++header_.slot_count;
			header_.free_start = static_cast<uint16_t>(header_.free_start + RSS_SLOTTED_PAGE_SLOT_BYTES);
			slot.generation = 1;
		} else {
			slot = readSlotLocked(slotId);
			++slot.generation;
		}
		if (valueBytes != 0U) {
			header_.free_end = static_cast<uint16_t>(header_.free_end - valueBytes);
			slot.offset = header_.free_end;
			slot.capacity = valueBytes;
			std::copy(value.begin(), value.end(), bytes_.begin() + slot.offset);
		} else {
			slot.offset = header_.free_end;
		}
		slot.value_bytes = valueBytes;
		slot.flags = RSS_SLOTTED_SLOT_FLAG_LIVE;
		writeSlotLocked(slotId, slot);
		refreshDerivedFieldsLocked();
		refreshHeaderLocked();
		return {header_.page_id, slotId, slot.generation};
	}

	std::vector<std::byte> SlottedPage::readLocked(const SlottedSlotReference &reference) const {
		validateReferenceLocked(reference);
		const Slot slot = readSlotLocked(reference.slot_id);
		return {bytes_.begin() + slot.offset, bytes_.begin() + slot.offset + slot.value_bytes};
	}

	void SlottedPage::overwriteLocked(const SlottedSlotReference &reference, std::span<const std::byte> value) {
		validateReferenceLocked(reference);
		const uint16_t valueBytes =
				checkedU16(value.size(), "relocatable slotted page record exceeds 16-bit slot capacity");
		Slot slot = readSlotLocked(reference.slot_id);
		if (valueBytes <= slot.capacity) {
			if (!value.empty()) {
				std::copy(value.begin(), value.end(), bytes_.begin() + slot.offset);
			}
			std::fill(bytes_.begin() + slot.offset + valueBytes, bytes_.begin() + slot.offset + slot.capacity,
					  std::byte{});
			slot.value_bytes = valueBytes;
			writeSlotLocked(reference.slot_id, slot);
		} else if (slot.offset == header_.free_end &&
				   static_cast<uint32_t>(valueBytes - slot.capacity) <= contiguousFreeLocked()) {
			const uint16_t growth = static_cast<uint16_t>(valueBytes - slot.capacity);
			header_.free_end = static_cast<uint16_t>(header_.free_end - growth);
			slot.offset = header_.free_end;
			slot.capacity = valueBytes;
			slot.value_bytes = valueBytes;
			std::copy(value.begin(), value.end(), bytes_.begin() + slot.offset);
			writeSlotLocked(reference.slot_id, slot);
		} else if (valueBytes <= contiguousFreeLocked()) {
			const uint16_t oldOffset = slot.offset;
			const uint16_t oldCapacity = slot.capacity;
			header_.free_end = static_cast<uint16_t>(header_.free_end - valueBytes);
			slot.offset = header_.free_end;
			slot.capacity = valueBytes;
			slot.value_bytes = valueBytes;
			std::copy(value.begin(), value.end(), bytes_.begin() + slot.offset);
			std::fill(bytes_.begin() + oldOffset, bytes_.begin() + oldOffset + oldCapacity, std::byte{});
			writeSlotLocked(reference.slot_id, slot);
		} else if (valueBytes <= contiguousFreeLocked() + header_.reclaimable_bytes + slot.capacity) {
			compactExcludingLocked(reference.slot_id);
			header_.free_end = static_cast<uint16_t>(header_.free_end - valueBytes);
			slot.offset = header_.free_end;
			slot.capacity = valueBytes;
			slot.value_bytes = valueBytes;
			std::copy(value.begin(), value.end(), bytes_.begin() + slot.offset);
			writeSlotLocked(reference.slot_id, slot);
		} else {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page cannot fit replacement value");
		}
		refreshDerivedFieldsLocked();
		refreshHeaderLocked();
	}

	void SlottedPage::eraseLocked(const SlottedSlotReference &reference) {
		validateReferenceLocked(reference);
		Slot slot = readSlotLocked(reference.slot_id);
		std::fill(bytes_.begin() + slot.offset, bytes_.begin() + slot.offset + slot.capacity, std::byte{});
		slot.offset = 0;
		slot.capacity = 0;
		slot.value_bytes = 0;
		slot.flags = 0;
		writeSlotLocked(reference.slot_id, slot);
		refreshDerivedFieldsLocked();
		refreshHeaderLocked();
	}

	void SlottedPage::compactLocked() { compactExcludingLocked(RSS_SLOTTED_PAGE_NO_SLOT); }

	void SlottedPage::compactExcludingLocked(uint16_t excludedSlot) {
		std::array<std::byte, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES> compacted{};
		uint16_t cursor = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			Slot slot = readSlotLocked(slotId);
			if (!slot.isLive() || slotId == excludedSlot) {
				continue;
			}
			cursor = static_cast<uint16_t>(cursor - slot.value_bytes);
			if (slot.value_bytes != 0U) {
				std::copy_n(bytes_.begin() + slot.offset, slot.value_bytes, compacted.begin() + cursor);
			}
			slot.offset = cursor;
			slot.capacity = slot.value_bytes;
			writeSlotLocked(slotId, slot);
		}
		if (excludedSlot != RSS_SLOTTED_PAGE_NO_SLOT) {
			Slot excluded = readSlotLocked(excludedSlot);
			excluded.offset = 0;
			excluded.capacity = 0;
			excluded.value_bytes = 0;
			excluded.flags = 0;
			writeSlotLocked(excludedSlot, excluded);
		}
		std::fill(bytes_.begin() + header_.free_start, bytes_.end(), std::byte{});
		std::copy(compacted.begin() + cursor, compacted.end(), bytes_.begin() + cursor);
		header_.free_end = cursor;
		refreshDerivedFieldsLocked();
		refreshHeaderLocked();
	}

	uint16_t SlottedPage::firstFreeSlotLocked() const {
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			if (!readSlotLocked(slotId).isLive()) {
				return slotId;
			}
		}
		return RSS_SLOTTED_PAGE_NO_SLOT;
	}

	uint16_t SlottedPage::firstReusableFreeSlotLocked() const {
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			const Slot slot = readSlotLocked(slotId);
			if (!slot.isLive() && slot.generation != std::numeric_limits<uint32_t>::max()) {
				return slotId;
			}
		}
		return RSS_SLOTTED_PAGE_NO_SLOT;
	}

	uint16_t SlottedPage::reusableFreeSlotCountLocked() const {
		uint16_t count = 0U;
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			const Slot slot = readSlotLocked(slotId);
			if (!slot.isLive() && slot.generation != std::numeric_limits<uint32_t>::max()) {
				++count;
			}
		}
		return count;
	}

	SlottedPage::Slot SlottedPage::readSlotLocked(uint16_t slotId) const {
		const size_t offset = slotOffset(slotId);
		return {LittleEndianCodec::readU16(bytes_, offset + SLOT_OFFSET_OFFSET),
				LittleEndianCodec::readU16(bytes_, offset + SLOT_CAPACITY_OFFSET),
				LittleEndianCodec::readU16(bytes_, offset + SLOT_VALUE_BYTES_OFFSET),
				LittleEndianCodec::readU16(bytes_, offset + SLOT_FLAGS_OFFSET),
				LittleEndianCodec::readU32(bytes_, offset + SLOT_GENERATION_OFFSET)};
	}

	void SlottedPage::writeSlotLocked(uint16_t slotId, const Slot &slot) {
		const size_t offset = slotOffset(slotId);
		LittleEndianCodec::writeU16(bytes_, offset + SLOT_OFFSET_OFFSET, slot.offset);
		LittleEndianCodec::writeU16(bytes_, offset + SLOT_CAPACITY_OFFSET, slot.capacity);
		LittleEndianCodec::writeU16(bytes_, offset + SLOT_VALUE_BYTES_OFFSET, slot.value_bytes);
		LittleEndianCodec::writeU16(bytes_, offset + SLOT_FLAGS_OFFSET, slot.flags);
		LittleEndianCodec::writeU32(bytes_, offset + SLOT_GENERATION_OFFSET, slot.generation);
		LittleEndianCodec::writeU32(bytes_, offset + SLOT_RESERVED_OFFSET, 0);
	}

	size_t SlottedPage::slotOffset(uint16_t slotId) const {
		if (slotId >= header_.slot_count) {
			throwSlottedPageError(FormatError::FERR_INVALID_HEADER, "relocatable slotted page slot ID is out of range");
		}
		return RSS_SLOTTED_PAGE_HEADER_BYTES + static_cast<size_t>(slotId) * RSS_SLOTTED_PAGE_SLOT_BYTES;
	}

	void SlottedPage::validateReferenceLocked(const SlottedSlotReference &reference) const {
		if (!reference.isValid() || reference.page_id != header_.page_id || reference.slot_id >= header_.slot_count) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page reference is invalid or foreign");
		}
		const Slot slot = readSlotLocked(reference.slot_id);
		if (!slot.isLive() || slot.generation != reference.generation) {
			throwSlottedPageError(FormatError::FERR_INVALID_STATE, "relocatable slotted page reference is stale");
		}
	}

	void SlottedPage::refreshHeaderLocked() {
		LittleEndianCodec::writeU32(bytes_, PAGE_MAGIC_OFFSET, RSS_SLOTTED_PAGE_MAGIC);
		LittleEndianCodec::writeU16(bytes_, PAGE_VERSION_OFFSET, RSS_SLOTTED_PAGE_VERSION);
		LittleEndianCodec::writeU16(bytes_, PAGE_HEADER_BYTES_OFFSET, RSS_SLOTTED_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes_, PAGE_ID_OFFSET, header_.page_id.value());
		LittleEndianCodec::writeU64(bytes_, PAGE_GENERATION_OFFSET, header_.page_generation.value());
		LittleEndianCodec::writeU16(bytes_, PAGE_SLOT_COUNT_OFFSET, header_.slot_count);
		LittleEndianCodec::writeU16(bytes_, PAGE_LIVE_SLOT_COUNT_OFFSET, header_.live_slot_count);
		LittleEndianCodec::writeU16(bytes_, PAGE_FREE_SLOT_HINT_OFFSET, header_.free_slot_hint);
		LittleEndianCodec::writeU16(bytes_, PAGE_RESERVED0_OFFSET, 0);
		LittleEndianCodec::writeU16(bytes_, PAGE_FREE_START_OFFSET, header_.free_start);
		LittleEndianCodec::writeU16(bytes_, PAGE_FREE_END_OFFSET, header_.free_end);
		LittleEndianCodec::writeU32(bytes_, PAGE_RESERVED1_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes_, PAGE_LIVE_BYTES_OFFSET, header_.live_bytes);
		LittleEndianCodec::writeU32(bytes_, PAGE_RECLAIMABLE_BYTES_OFFSET, header_.reclaimable_bytes);
		LittleEndianCodec::writeU32(bytes_, PAGE_FLAGS_OFFSET, RSS_SLOTTED_PAGE_KNOWN_FLAGS);
		LittleEndianCodec::writeU32(bytes_, PAGE_CRC_OFFSET, 0);
		LittleEndianCodec::writeU64(bytes_, PAGE_RESERVED2_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes_, PAGE_CRC_OFFSET, calculateCrc(bytes_));
	}

	void SlottedPage::refreshDerivedFieldsLocked() {
		uint32_t liveSlots = 0;
		uint32_t liveBytes = 0;
		uint32_t allocatedBytes = 0;
		for (uint16_t slotId = 0; slotId < header_.slot_count; ++slotId) {
			const Slot slot = readSlotLocked(slotId);
			if (!slot.isLive()) {
				continue;
			}
			++liveSlots;
			liveBytes += slot.value_bytes;
			allocatedBytes += slot.capacity;
		}
		header_.live_slot_count = static_cast<uint16_t>(liveSlots);
		header_.live_bytes = liveBytes;
		header_.reclaimable_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - header_.free_end - allocatedBytes;
		header_.free_slot_hint = firstFreeSlotLocked();
	}

	uint32_t SlottedPage::contiguousFreeLocked() const { return header_.free_end - header_.free_start; }

} // namespace graph::storage
