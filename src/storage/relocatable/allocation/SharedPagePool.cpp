/**
 * @file SharedPagePool.cpp
 * @brief Checked codecs and frame-map operations for relocatable shared page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"

#include <algorithm>
#include <bit>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t POOL_MAGIC_OFFSET = 0;
		constexpr size_t POOL_VERSION_OFFSET = 4;
		constexpr size_t POOL_HEADER_BYTES_OFFSET = 6;
		constexpr size_t POOL_SEGMENT_ID_OFFSET = 8;
		constexpr size_t POOL_ALLOCATED_FRAMES_OFFSET = 16;
		constexpr size_t POOL_ALLOCATED_COUNT_OFFSET = 20;
		constexpr size_t POOL_GENERATION_OFFSET = 24;
		constexpr size_t POOL_LSN_OFFSET = 32;
		constexpr size_t POOL_HEADER_CRC_OFFSET = 40;
		constexpr size_t POOL_RESERVED0_OFFSET = 44;
		constexpr size_t POOL_RESERVED1_OFFSET = 48;
		constexpr size_t POOL_RESERVED2_OFFSET = 56;

		constexpr size_t OWNER_MAGIC_OFFSET = 0;
		constexpr size_t OWNER_VERSION_OFFSET = 4;
		constexpr size_t OWNER_HEADER_BYTES_OFFSET = 6;
		constexpr size_t OWNER_PAGE_ID_OFFSET = 8;
		constexpr size_t OWNER_ID_OFFSET = 16;
		constexpr size_t OWNER_KIND_OFFSET = 24;
		constexpr size_t OWNER_FLAGS_OFFSET = 26;
		constexpr size_t OWNER_PAYLOAD_BYTES_OFFSET = 28;
		constexpr size_t OWNER_GENERATION_OFFSET = 32;
		constexpr size_t OWNER_LSN_OFFSET = 40;
		constexpr size_t OWNER_HEADER_CRC_OFFSET = 48;
		constexpr size_t OWNER_RESERVED0_OFFSET = 52;
		constexpr size_t OWNER_RESERVED1_OFFSET = 56;

		[[noreturn]] void throwSharedPagePoolError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isPageOwnerKind(PageOwnerKind kind) {
			switch (kind) {
				case PageOwnerKind::POK_PROPERTY:
				case PageOwnerKind::POK_INDEX:
				case PageOwnerKind::POK_VECTOR_RAW:
				case PageOwnerKind::POK_VECTOR_PQ:
				case PageOwnerKind::POK_VECTOR_ADJACENCY:
				case PageOwnerKind::POK_VECTOR_MANIFEST:
				case PageOwnerKind::POK_STATE:
				case PageOwnerKind::POK_ENTITY:
				case PageOwnerKind::POK_ENTITY_LABEL:
				case PageOwnerKind::POK_LARGE_VALUE_DATA:
				case PageOwnerKind::POK_LARGE_VALUE_MANIFEST:
					return true;
				case PageOwnerKind::POK_NONE:
					return false;
			}
			return false;
		}

		void validatePoolHeader(const SharedPagePoolHeader &header) {
			const uint8_t invalid =
					static_cast<uint8_t>(!header.segment_id.isValid()) |
					static_cast<uint8_t>(!header.generation.isValid()) |
					static_cast<uint8_t>(!header.committed_lsn.isValid()) |
					static_cast<uint8_t>((header.allocated_frames & ~RSS_SHARED_PAGE_POOL_VALID_FRAME_MASK) != 0U) |
					static_cast<uint8_t>(static_cast<uint32_t>(std::popcount(header.allocated_frames)) >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalid != 0U) {
				throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER, "invalid relocatable shared page-pool header");
			}
		}

		void validateOwnerHeader(const PageOwnerHeader &header) {
			const uint8_t invalid = static_cast<uint8_t>(!header.page_id.isValid()) |
									static_cast<uint8_t>(!isPageOwnerKind(header.kind)) |
									static_cast<uint8_t>(header.payload_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(!header.generation.isValid()) |
									static_cast<uint8_t>(!header.committed_lsn.isValid());
			if (invalid != 0U) {
				throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER, "invalid relocatable page-owner header");
			}
			if ((header.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) {
				throwSharedPagePoolError(FormatError::FERR_RESERVED_BITS, "relocatable page-owner header uses unknown flags");
			}
		}

		void validateFrameIndex(uint8_t frameIndex) {
			if (frameIndex < RSS_PAGE_POOL_FIRST_DATA_FRAME || frameIndex > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
				throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER, "relocatable shared page-pool frame is invalid");
			}
		}

		[[nodiscard]] uint32_t frameBit(uint8_t frameIndex) {
			validateFrameIndex(frameIndex);
			return 1U << (frameIndex - RSS_PAGE_POOL_FIRST_DATA_FRAME);
		}

		void writePoolHeader(std::span<std::byte> bytes, const SharedPagePoolHeader &header, uint32_t checksum) {
			LittleEndianCodec::writeU32(bytes, POOL_MAGIC_OFFSET, RSS_SHARED_PAGE_POOL_MAGIC);
			LittleEndianCodec::writeU16(bytes, POOL_VERSION_OFFSET, RSS_SHARED_PAGE_POOL_VERSION);
			LittleEndianCodec::writeU16(bytes, POOL_HEADER_BYTES_OFFSET, RSS_SHARED_PAGE_POOL_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, POOL_SEGMENT_ID_OFFSET, header.segment_id.value());
			LittleEndianCodec::writeU32(bytes, POOL_ALLOCATED_FRAMES_OFFSET, header.allocated_frames);
			LittleEndianCodec::writeU32(bytes, POOL_ALLOCATED_COUNT_OFFSET, std::popcount(header.allocated_frames));
			LittleEndianCodec::writeU64(bytes, POOL_GENERATION_OFFSET, header.generation.value());
			LittleEndianCodec::writeU64(bytes, POOL_LSN_OFFSET, header.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, POOL_HEADER_CRC_OFFSET, checksum);
			LittleEndianCodec::writeU32(bytes, POOL_RESERVED0_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, POOL_RESERVED1_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, POOL_RESERVED2_OFFSET, 0);
		}

		void writeOwnerHeader(std::span<std::byte> bytes, const PageOwnerHeader &header, uint32_t checksum) {
			LittleEndianCodec::writeU32(bytes, OWNER_MAGIC_OFFSET, RSS_PAGE_OWNER_MAGIC);
			LittleEndianCodec::writeU16(bytes, OWNER_VERSION_OFFSET, RSS_PAGE_OWNER_VERSION);
			LittleEndianCodec::writeU16(bytes, OWNER_HEADER_BYTES_OFFSET, RSS_PAGE_OWNER_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, OWNER_PAGE_ID_OFFSET, header.page_id.value());
			LittleEndianCodec::writeU64(bytes, OWNER_ID_OFFSET, header.owner_id);
			LittleEndianCodec::writeU16(bytes, OWNER_KIND_OFFSET, static_cast<uint16_t>(header.kind));
			LittleEndianCodec::writeU16(bytes, OWNER_FLAGS_OFFSET, header.flags);
			LittleEndianCodec::writeU32(bytes, OWNER_PAYLOAD_BYTES_OFFSET, header.payload_bytes);
			LittleEndianCodec::writeU64(bytes, OWNER_GENERATION_OFFSET, header.generation.value());
			LittleEndianCodec::writeU64(bytes, OWNER_LSN_OFFSET, header.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, OWNER_HEADER_CRC_OFFSET, checksum);
			LittleEndianCodec::writeU32(bytes, OWNER_RESERVED0_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, OWNER_RESERVED1_OFFSET, 0);
		}

	} // namespace

	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES>
	SharedPagePoolHeaderCodec::encode(const SharedPagePoolHeader &header) {
		validatePoolHeader(header);
		std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> bytes{};
		writePoolHeader(bytes, header, 0);
		writePoolHeader(bytes, header, calculateCrc(bytes));
		return bytes;
	}

	SharedPagePoolHeader SharedPagePoolHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_SHARED_PAGE_POOL_HEADER_BYTES) {
			throwSharedPagePoolError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable shared page-pool header is truncated");
		}
		if (bytes.size() != RSS_SHARED_PAGE_POOL_HEADER_BYTES) {
			throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER,
									 "relocatable shared page-pool header has an unexpected size");
		}
		std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES> checksumBytes{};
		std::copy_n(bytes.begin(), checksumBytes.size(), checksumBytes.begin());
		const uint32_t expectedChecksum = LittleEndianCodec::readU32(checksumBytes, POOL_HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(checksumBytes, POOL_HEADER_CRC_OFFSET, 0);
		if (calculateCrc(checksumBytes) != expectedChecksum) {
			throwSharedPagePoolError(FormatError::FERR_CHECKSUM_MISMATCH,
									 "relocatable shared page-pool header checksum does not match");
		}
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, POOL_MAGIC_OFFSET) !=
									 RSS_SHARED_PAGE_POOL_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, POOL_VERSION_OFFSET) !=
									 RSS_SHARED_PAGE_POOL_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, POOL_HEADER_BYTES_OFFSET) !=
									 RSS_SHARED_PAGE_POOL_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, POOL_RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, POOL_RESERVED1_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, POOL_RESERVED2_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			throwSharedPagePoolError(FormatError::FERR_RESERVED_BITS,
									 "relocatable shared page-pool header has invalid fixed fields");
		}
		SharedPagePoolHeader header;
		header.segment_id = SegmentId(LittleEndianCodec::readU64(bytes, POOL_SEGMENT_ID_OFFSET));
		header.allocated_frames = LittleEndianCodec::readU32(bytes, POOL_ALLOCATED_FRAMES_OFFSET);
		header.generation = Generation(LittleEndianCodec::readU64(bytes, POOL_GENERATION_OFFSET));
		header.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, POOL_LSN_OFFSET));
		validatePoolHeader(header);
		if (LittleEndianCodec::readU32(bytes, POOL_ALLOCATED_COUNT_OFFSET) !=
			static_cast<uint32_t>(std::popcount(header.allocated_frames))) {
			throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER,
									 "relocatable shared page-pool frame count does not match its bitmap");
		}
		return header;
	}

	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> PageOwnerHeaderCodec::encode(const PageOwnerHeader &header) {
		validateOwnerHeader(header);
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
		writeOwnerHeader(bytes, header, 0);
		writeOwnerHeader(bytes, header, calculateCrc(bytes));
		return bytes;
	}

	PageOwnerHeader PageOwnerHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_OWNER_HEADER_BYTES) {
			throwSharedPagePoolError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable page-owner header is truncated");
		}
		if (bytes.size() != RSS_PAGE_OWNER_HEADER_BYTES) {
			throwSharedPagePoolError(FormatError::FERR_INVALID_HEADER,
									 "relocatable page-owner header has an unexpected size");
		}
		std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> checksumBytes{};
		std::copy_n(bytes.begin(), checksumBytes.size(), checksumBytes.begin());
		const uint32_t expectedChecksum = LittleEndianCodec::readU32(checksumBytes, OWNER_HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(checksumBytes, OWNER_HEADER_CRC_OFFSET, 0);
		if (calculateCrc(checksumBytes) != expectedChecksum) {
			throwSharedPagePoolError(FormatError::FERR_CHECKSUM_MISMATCH,
									 "relocatable page-owner header checksum does not match");
		}
		const uint16_t rawKind = LittleEndianCodec::readU16(bytes, OWNER_KIND_OFFSET);
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, OWNER_MAGIC_OFFSET) != RSS_PAGE_OWNER_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, OWNER_VERSION_OFFSET) !=
									 RSS_PAGE_OWNER_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, OWNER_HEADER_BYTES_OFFSET) !=
									 RSS_PAGE_OWNER_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, OWNER_RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, OWNER_RESERVED1_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			throwSharedPagePoolError(FormatError::FERR_RESERVED_BITS,
									 "relocatable page-owner header has invalid fixed fields");
		}
		PageOwnerHeader header;
		header.page_id = PageId(LittleEndianCodec::readU64(bytes, OWNER_PAGE_ID_OFFSET));
		header.owner_id = LittleEndianCodec::readU64(bytes, OWNER_ID_OFFSET);
		header.kind = static_cast<PageOwnerKind>(rawKind);
		header.flags = LittleEndianCodec::readU16(bytes, OWNER_FLAGS_OFFSET);
		header.payload_bytes = LittleEndianCodec::readU32(bytes, OWNER_PAYLOAD_BYTES_OFFSET);
		header.generation = Generation(LittleEndianCodec::readU64(bytes, OWNER_GENERATION_OFFSET));
		header.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, OWNER_LSN_OFFSET));
		validateOwnerHeader(header);
		return header;
	}

	std::optional<uint8_t> SharedPagePoolFrameMap::allocateFirstFree(SharedPagePoolHeader &header) {
		validatePoolHeader(header);
		const uint32_t freeFrames = RSS_SHARED_PAGE_POOL_VALID_FRAME_MASK & ~header.allocated_frames;
		if (freeFrames == 0U) {
			return std::nullopt;
		}
		const uint8_t frameIndex =
				static_cast<uint8_t>(std::countr_zero(freeFrames) + RSS_PAGE_POOL_FIRST_DATA_FRAME);
		header.allocated_frames |= frameBit(frameIndex);
		return frameIndex;
	}

	void SharedPagePoolFrameMap::release(SharedPagePoolHeader &header, uint8_t frameIndex) {
		validatePoolHeader(header);
		const uint32_t bit = frameBit(frameIndex);
		if ((header.allocated_frames & bit) == 0U) {
			throwSharedPagePoolError(FormatError::FERR_INVALID_STATE,
									 "relocatable shared page-pool release targets an unallocated frame");
		}
		header.allocated_frames &= ~bit;
	}

	bool SharedPagePoolFrameMap::isAllocated(const SharedPagePoolHeader &header, uint8_t frameIndex) {
		validatePoolHeader(header);
		return (header.allocated_frames & frameBit(frameIndex)) != 0U;
	}

} // namespace graph::storage
