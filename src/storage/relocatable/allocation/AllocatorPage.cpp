/**
 * @file AllocatorPage.cpp
 * @brief Explicit codec for relocatable physical-slot bitmap leaf pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t BITMAP_BYTES_OFFSET = 8;
		constexpr size_t SLOT_COUNT_OFFSET = 12;
		constexpr size_t ALLOCATED_COUNT_OFFSET = 16;
		constexpr size_t FLAGS_OFFSET = 20;
		constexpr size_t PAGE_ID_OFFSET = 24;
		constexpr size_t FIRST_SLOT_OFFSET = 32;
		constexpr size_t GENERATION_OFFSET = 40;
		constexpr size_t COMMITTED_LSN_OFFSET = 48;
		constexpr size_t PAYLOAD_CRC_OFFSET = 56;
		constexpr size_t HEADER_CRC_OFFSET = 60;

		[[noreturn]] void throwAllocatorPageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] uint32_t allocatedCount(const AllocatorLeafPage &page) {
			uint32_t result = 0;
			for (const std::byte value: page.allocated_slots) {
				result += std::popcount(std::to_integer<uint8_t>(value));
			}
			return result;
		}

		void validatePage(const AllocatorLeafPage &page) {
			const uint8_t invalid = static_cast<uint8_t>(!page.page_id.isValid()) |
									static_cast<uint8_t>(!page.first_slot.isValid()) |
									static_cast<uint8_t>(page.slot_count == 0U) |
									static_cast<uint8_t>(page.slot_count > RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) |
									static_cast<uint8_t>(!page.generation.isValid()) |
									static_cast<uint8_t>(!page.committed_lsn.isValid()) |
									static_cast<uint8_t>(page.first_slot.value() >
														 std::numeric_limits<uint64_t>::max() - (page.slot_count - 1U));
			if (invalid != 0U) {
				throwAllocatorPageError(FormatError::FERR_INVALID_HEADER, "invalid relocatable free-space leaf header");
			}

			const uint32_t usedBytes = (page.slot_count + 7U) / 8U;
			const uint32_t remainderBits = page.slot_count % 8U;
			if (remainderBits != 0U) {
				const uint8_t unusedMask = static_cast<uint8_t>(~((1U << remainderBits) - 1U));
				if ((std::to_integer<uint8_t>(page.allocated_slots[usedBytes - 1U]) & unusedMask) != 0U) {
					throwAllocatorPageError(FormatError::FERR_RESERVED_BITS,
											"relocatable free-space leaf sets bits outside its slot range");
				}
			}
			for (uint32_t index = usedBytes; index < RSS_ALLOCATOR_PAGE_BITMAP_BYTES; ++index) {
				if (page.allocated_slots[index] != std::byte{}) {
					throwAllocatorPageError(FormatError::FERR_RESERVED_BITS,
											"relocatable free-space leaf has nonzero unused bitmap bytes");
				}
			}
		}

		void writeHeader(std::span<std::byte> bytes, const AllocatorLeafPage &page, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_ALLOCATOR_PAGE_MAGIC);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, RSS_ALLOCATOR_PAGE_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_ALLOCATOR_PAGE_HEADER_BYTES);
			LittleEndianCodec::writeU32(bytes, BITMAP_BYTES_OFFSET, RSS_ALLOCATOR_PAGE_BITMAP_BYTES);
			LittleEndianCodec::writeU32(bytes, SLOT_COUNT_OFFSET, page.slot_count);
			LittleEndianCodec::writeU32(bytes, ALLOCATED_COUNT_OFFSET, allocatedCount(page));
			LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, page.page_id.value());
			LittleEndianCodec::writeU64(bytes, FIRST_SLOT_OFFSET, page.first_slot.value());
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, page.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, page.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
		}

	} // namespace

	std::array<std::byte, RSS_PAGE_SIZE> AllocatorLeafPageCodec::encode(const AllocatorLeafPage &page) {
		validatePage(page);
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		std::copy(page.allocated_slots.begin(), page.allocated_slots.end(),
				  bytes.begin() + RSS_ALLOCATOR_PAGE_HEADER_BYTES);
		writeHeader(bytes, page, calculateCrc(std::span(bytes).subspan(RSS_ALLOCATOR_PAGE_HEADER_BYTES)), 0);
		writeHeader(bytes, page, LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET),
					calculateCrc(std::span(bytes).first(RSS_ALLOCATOR_PAGE_HEADER_BYTES)));
		return bytes;
	}

	AllocatorLeafPage AllocatorLeafPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwAllocatorPageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable free-space leaf is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwAllocatorPageError(FormatError::FERR_INVALID_HEADER, "relocatable free-space leaf has an unexpected size");
		}

		std::array<std::byte, RSS_ALLOCATOR_PAGE_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwAllocatorPageError(FormatError::FERR_CHECKSUM_MISMATCH,
									"relocatable free-space leaf header checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_ALLOCATOR_PAGE_MAGIC) {
			throwAllocatorPageError(FormatError::FERR_INVALID_MAGIC, "relocatable free-space leaf magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET) != RSS_ALLOCATOR_PAGE_VERSION) {
			throwAllocatorPageError(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported relocatable free-space leaf version");
		}
		const uint8_t invalidDimensions = static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
															   RSS_ALLOCATOR_PAGE_HEADER_BYTES) |
										  static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, BITMAP_BYTES_OFFSET) !=
															   RSS_ALLOCATOR_PAGE_BITMAP_BYTES) |
										  static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) != 0U);
		if (invalidDimensions != 0U) {
			throwAllocatorPageError(FormatError::FERR_RESERVED_BITS,
									"relocatable free-space leaf uses invalid dimensions or flags");
		}
		if (calculateCrc(bytes.subspan(RSS_ALLOCATOR_PAGE_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwAllocatorPageError(FormatError::FERR_CHECKSUM_MISMATCH,
									"relocatable free-space leaf payload checksum does not match");
		}

		AllocatorLeafPage page;
		page.page_id = MetadataPageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		page.first_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, FIRST_SLOT_OFFSET));
		page.slot_count = LittleEndianCodec::readU32(bytes, SLOT_COUNT_OFFSET);
		page.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		page.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		std::copy_n(bytes.begin() + RSS_ALLOCATOR_PAGE_HEADER_BYTES, page.allocated_slots.size(),
					page.allocated_slots.begin());
		validatePage(page);
		if (LittleEndianCodec::readU32(bytes, ALLOCATED_COUNT_OFFSET) != allocatedCount(page)) {
			throwAllocatorPageError(FormatError::FERR_INVALID_HEADER,
									"relocatable free-space leaf allocation count does not match its bitmap");
		}
		return page;
	}

} // namespace graph::storage
