/**
 * @file Format.hpp
 * @brief Explicit, versioned primitives for the experimental relocatable format.
 *
 * No type in this header is persisted through its native C++ representation.
 * Every disk representation is emitted and consumed by LittleEndianCodec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace graph::storage {

	constexpr uint32_t RSS_SEGMENT_SIZE = 128U * 1024U;
	constexpr uint32_t RSS_PAGE_SIZE = 4U * 1024U;
	constexpr uint32_t RSS_FILE_BOOTSTRAP_BYTES = RSS_SEGMENT_SIZE;
	// The first logical Segment ID is owned by the bootstrap metadata page pool.
	// The following ID is permanently assigned to the bootstrap-addressed metadata
	// emergency reserve.  It is not Catalog-visible until that reserve is rotated
	// into normal service, so user/data allocation starts after both bootstrap
	// identities.  Keeping the reserve token permanent avoids mutating a live
	// pool header during rotation.
	constexpr uint64_t RSS_BOOTSTRAP_METADATA_SEGMENT_ID = 1U;
	constexpr uint64_t RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN =
			RSS_BOOTSTRAP_METADATA_SEGMENT_ID + 1U;
	constexpr uint64_t RSS_FIRST_ALLOCATABLE_SEGMENT_ID = RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN + 1U;
	constexpr uint32_t RSS_PAGE_POOL_FIRST_DATA_FRAME = 1;
	constexpr uint32_t RSS_PAGE_POOL_DATA_FRAME_COUNT = RSS_SEGMENT_SIZE / RSS_PAGE_SIZE - 1U;
	constexpr uint16_t RSS_FORMAT_VERSION = 5;
	constexpr uint32_t RSS_SEGMENT_MAGIC = 0x4758595AU; // Little-endian bytes: ZYXG
	constexpr uint32_t RSS_ENVELOPE_BYTES = 64;
	// The 16-bit envelope field is constrained to the largest 8-byte-aligned value.
	constexpr uint16_t RSS_MAX_HEADER_BYTES = 65528U;
	constexpr uint32_t RSS_ENVELOPE_KNOWN_FLAGS = 0;
	static_assert(RSS_MAX_HEADER_BYTES <= RSS_SEGMENT_SIZE);

	template<typename Tag>
	class StrongUInt64 {
	public:
		constexpr StrongUInt64() = default;
		explicit constexpr StrongUInt64(uint64_t value) : value_(value) {}

		[[nodiscard]] constexpr uint64_t value() const { return value_; }
		[[nodiscard]] constexpr bool isValid() const { return value_ != 0; }
		constexpr auto operator<=>(const StrongUInt64 &) const = default;

	private:
		uint64_t value_ = 0;
	};

	struct SegmentIdTag {};
	struct DatabaseIdTag {};
	struct PhysicalSlotIdTag {};
	struct PageIdTag {};
	// Metadata COW pages have an identity space independent from data-page
	// PageId. The wire representation remains a uint64_t, but callers must not
	// accidentally resolve one namespace through the other's directory.
	struct MetadataPageIdTag {};
	struct LargeValueIdTag {};
	struct ExtentIdTag {};
	struct GenerationTag {};
	struct LogSequenceNumberTag {};

	using SegmentId = StrongUInt64<SegmentIdTag>;
	using DatabaseId = StrongUInt64<DatabaseIdTag>;
	using PhysicalSlotId = StrongUInt64<PhysicalSlotIdTag>;
	using PageId = StrongUInt64<PageIdTag>;
	using MetadataPageId = StrongUInt64<MetadataPageIdTag>;
	using LargeValueId = StrongUInt64<LargeValueIdTag>;
	using ExtentId = StrongUInt64<ExtentIdTag>;
	using Generation = StrongUInt64<GenerationTag>;
	using LogSequenceNumber = StrongUInt64<LogSequenceNumberTag>;

	static_assert(!std::is_convertible_v<PageId, MetadataPageId>);
	static_assert(!std::is_convertible_v<MetadataPageId, PageId>);

	// Physical references are restricted to relocatable metadata. Business records
	// persist stable logical IDs and resolve them through directories instead.
	struct PhysicalPageRef {
		PhysicalSlotId physical_slot{};
		uint8_t frame_index = 0;
		Generation generation{};

		[[nodiscard]] constexpr bool isValid() const {
			return physical_slot.isValid() && frame_index >= RSS_PAGE_POOL_FIRST_DATA_FRAME &&
				   frame_index <= RSS_PAGE_POOL_DATA_FRAME_COUNT && generation.isValid();
		}
		constexpr auto operator<=>(const PhysicalPageRef &) const = default;
	};

	// All enumerators use prefixes so they remain safe in Windows builds.
	enum class SegmentKind : uint16_t {
		SGK_NONE = 0,
		// Shared Node, Edge, and overflow-label pages. PageOwnerKind selects
		// the fixed or slotted payload codec for each 4 KiB frame.
		SGK_ENTITY_PAGE_POOL = 1,
		SGK_PROPERTY_PAGE_POOL = 2,
		SGK_INDEX_PAGE_POOL = 3,
		SGK_VECTOR_RAW_POOL = 4,
		SGK_VECTOR_PQ_POOL = 5,
		SGK_VECTOR_ADJACENCY_POOL = 6,
		SGK_CODEBOOK_EXTENT = 8,
		SGK_METADATA_PAGE_POOL = 9,
		SGK_VECTOR_MANIFEST_POOL = 10,
		SGK_STATE_PAGE_POOL = 11,
		// Generic external values use typed 4 KiB data and manifest frames.
		SGK_LARGE_VALUE_DATA_POOL = 12,
		SGK_LARGE_VALUE_MANIFEST_POOL = 13,
		// Transaction-private bounded-sort fragments. These segments never hold
		// public graph state and writable open reclaims every leftover fragment
		// before exposing the backend to readers.
		SGK_BULK_SCRATCH = 14,
		// Query-owned, graph-unreachable scratch; never reclaimed as bulk scratch.
		SGK_QUERY_SCRATCH = 15,
	};

	enum class SegmentState : uint16_t {
		SGS_FREE = 0,
		SGS_BUILDING = 1,
		SGS_SEALED = 2,
		SGS_RETIRED = 3,
		SGS_OBSOLETE = 4,
	};

	enum class FormatError : uint8_t {
		FERR_BUFFER_TOO_SMALL = 1,
		FERR_INVALID_MAGIC = 2,
		FERR_UNSUPPORTED_VERSION = 3,
		FERR_INVALID_ENUM = 4,
		FERR_INVALID_HEADER = 5,
		FERR_RESERVED_BITS = 6,
		FERR_CHECKSUM_MISMATCH = 7,
		FERR_ARITHMETIC_OVERFLOW = 8,
		FERR_INVALID_STATE = 9,
	};

	class FormatException final : public std::runtime_error {
	public:
		FormatException(FormatError error, const std::string &message);
		[[nodiscard]] FormatError error() const { return error_; }

	private:
		FormatError error_;
	};

	class LittleEndianCodec {
	public:
		static void writeU16(std::span<std::byte> bytes, size_t offset, uint16_t value);
		static void writeU32(std::span<std::byte> bytes, size_t offset, uint32_t value);
		static void writeU64(std::span<std::byte> bytes, size_t offset, uint64_t value);

		[[nodiscard]] static uint16_t readU16(std::span<const std::byte> bytes, size_t offset);
		[[nodiscard]] static uint32_t readU32(std::span<const std::byte> bytes, size_t offset);
		[[nodiscard]] static uint64_t readU64(std::span<const std::byte> bytes, size_t offset);
	};

	struct SegmentEnvelope {
		uint32_t magic = RSS_SEGMENT_MAGIC;
		uint16_t format_version = RSS_FORMAT_VERSION;
		uint16_t header_bytes = RSS_ENVELOPE_BYTES;
		SegmentKind kind = SegmentKind::SGK_NONE;
		SegmentState state = SegmentState::SGS_FREE;
		uint32_t flags = 0;
		SegmentId segment_id{};
		uint64_t owner_id = 0;
		Generation generation{};
		LogSequenceNumber committed_lsn{};
		uint32_t payload_bytes = 0;
		uint32_t header_crc = 0;
		uint32_t payload_crc = 0;
		uint32_t reserved = 0;
	};

	class SegmentEnvelopeCodec {
	public:
		[[nodiscard]] static std::array<std::byte, RSS_ENVELOPE_BYTES> encode(const SegmentEnvelope &envelope);
		[[nodiscard]] static SegmentEnvelope decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
