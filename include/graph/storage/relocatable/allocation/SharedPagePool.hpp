/**
 * @file SharedPagePool.hpp
 * @brief Explicit page-pool headers and checked frame-map operations for relocatable.
 *
 * A shared pool occupies one fixed physical segment. Frame zero keeps pool
 * metadata and every remaining 4 KiB frame begins with an owner header, so
 * multiple logical owners can share an index or property pool without
 * scanning payload records to determine ownership.
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

namespace graph::storage {

	constexpr uint32_t RSS_SHARED_PAGE_POOL_MAGIC = 0x5053595AU; // Little-endian bytes: ZYSP
	constexpr uint16_t RSS_SHARED_PAGE_POOL_VERSION = 2;
	constexpr uint16_t RSS_SHARED_PAGE_POOL_HEADER_BYTES = 64;
	constexpr uint16_t RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT = 2;
	constexpr uint16_t RSS_SHARED_PAGE_POOL_CONTROL_RECORD_BYTES =
			RSS_SHARED_PAGE_POOL_HEADER_BYTES * RSS_SHARED_PAGE_POOL_CONTROL_RECORD_COUNT;
	constexpr uint32_t RSS_SHARED_PAGE_POOL_VALID_FRAME_MASK = (1U << RSS_PAGE_POOL_DATA_FRAME_COUNT) - 1U;

	constexpr uint32_t RSS_PAGE_OWNER_MAGIC = 0x4F50595AU; // Little-endian bytes: ZYPO
	constexpr uint16_t RSS_PAGE_OWNER_VERSION = 2;
	constexpr uint16_t RSS_PAGE_OWNER_HEADER_BYTES = 64;
	constexpr uint32_t RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES = RSS_PAGE_SIZE - RSS_PAGE_OWNER_HEADER_BYTES;
	constexpr uint16_t RSS_PAGE_OWNER_FLAG_RETIRED = 1U;
	constexpr uint16_t RSS_PAGE_OWNER_KNOWN_FLAGS = RSS_PAGE_OWNER_FLAG_RETIRED;

	[[nodiscard]] constexpr bool isSharedPagePoolSegmentKind(SegmentKind kind) {
		switch (kind) {
			case SegmentKind::SGK_ENTITY_PAGE_POOL:
			case SegmentKind::SGK_PROPERTY_PAGE_POOL:
			case SegmentKind::SGK_INDEX_PAGE_POOL:
			case SegmentKind::SGK_VECTOR_RAW_POOL:
			case SegmentKind::SGK_VECTOR_PQ_POOL:
			case SegmentKind::SGK_VECTOR_ADJACENCY_POOL:
			case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
			case SegmentKind::SGK_STATE_PAGE_POOL:
			case SegmentKind::SGK_LARGE_VALUE_DATA_POOL:
			case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL:
				return true;
			default:
				return false;
		}
	}

	// All enumerators use a prefix so Windows macros cannot collide with them.
	enum class PageOwnerKind : uint16_t {
		POK_NONE = 0,
		POK_PROPERTY = 1,
		POK_INDEX = 2,
		POK_VECTOR_RAW = 3,
		POK_VECTOR_PQ = 4,
		POK_VECTOR_ADJACENCY = 5,
		POK_VECTOR_MANIFEST = 7,
		POK_STATE = 8,
		POK_ENTITY = 9,
		// A Node keeps only the small label prefix in its fixed record. The
		// remaining canonical labels live in a checked slotted-page record.
		POK_ENTITY_LABEL = 10,
		POK_LARGE_VALUE_DATA = 11,
		POK_LARGE_VALUE_MANIFEST = 12,
	};

	struct SharedPagePoolHeader {
		SegmentId segment_id{};
		uint32_t allocated_frames = 0;
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		uint64_t control_sequence = 0U;
	};

	class SharedPagePoolHeaderCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES>
		encode(const SharedPagePoolHeader &header);
		[[nodiscard]] static SharedPagePoolHeader decode(std::span<const std::byte> bytes);
	};

	struct PageOwnerHeader {
		PageId page_id{};
		uint64_t owner_id = 0;
		PageOwnerKind kind = PageOwnerKind::POK_NONE;
		uint16_t flags = 0;
		uint32_t payload_bytes = 0;
		Generation generation{};
		LogSequenceNumber committed_lsn{};
	};

	class PageOwnerHeaderCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> encode(const PageOwnerHeader &header);
		[[nodiscard]] static PageOwnerHeader decode(std::span<const std::byte> bytes);
	};

	// These operations mutate only an in-memory header. Their caller must make
	// the header and page-owner writes durable before publishing Page Directory
	// reachability, then let recovery clear bitmap bits with no reachable owner.
	class SharedPagePoolFrameMap {
	public:
		[[nodiscard]] static std::optional<uint8_t> allocateFirstFree(SharedPagePoolHeader &header);
		static void release(SharedPagePoolHeader &header, uint8_t frameIndex);
		[[nodiscard]] static bool isAllocated(const SharedPagePoolHeader &header, uint8_t frameIndex);
	};

} // namespace graph::storage
