/**
 * @file MetadataPageRoute.cpp
 * @brief Canonical codec for MetadataPageId to PhysicalPageRef routes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPageRoute.hpp"

#include <algorithm>
#include <limits>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t PAGE_ID_OFFSET = 8U;
		constexpr size_t GENERATION_OFFSET = 16U;
		constexpr size_t COMMITTED_LSN_OFFSET = 24U;
		constexpr size_t ENTRY_COUNT_OFFSET = 32U;
		constexpr size_t ENTRY_BYTES_OFFSET = 36U;
		constexpr size_t PAYLOAD_CRC_OFFSET = 40U;
		constexpr size_t HEADER_CRC_OFFSET = 44U;
		constexpr size_t RESERVED_OFFSET = 48U;
		constexpr size_t ENTRY_PAGE_ID_OFFSET = 0U;
		constexpr size_t ENTRY_SLOT_OFFSET = 8U;
		constexpr size_t ENTRY_PACKED_REFERENCE_OFFSET = 16U;
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[noreturn]] void throwRouteError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateReference(const PhysicalPageRef &reference) {
			if (!reference.isValid() || reference.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) {
				throwRouteError(FormatError::FERR_INVALID_HEADER,
								"metadata PageId route has an invalid physical reference");
			}
		}

		void writeReference(std::span<std::byte> bytes, size_t offset, const PhysicalPageRef &reference) {
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_SLOT_OFFSET, reference.physical_slot.value());
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_PACKED_REFERENCE_OFFSET,
									 (reference.generation.value() << 5U) | reference.frame_index);
		}

		[[nodiscard]] PhysicalPageRef readReference(std::span<const std::byte> bytes, size_t offset) {
			PhysicalPageRef reference;
			reference.physical_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, offset + ENTRY_SLOT_OFFSET));
			const uint64_t packed = LittleEndianCodec::readU64(bytes, offset + ENTRY_PACKED_REFERENCE_OFFSET);
			reference.frame_index = static_cast<uint8_t>(packed & 0x1FU);
			reference.generation = Generation(packed >> 5U);
			return reference;
		}

		void validatePage(const MetadataPageRouteLeafPage &page) {
			if (!page.page_id.isValid() || !page.generation.isValid() || !page.committed_lsn.isValid() ||
				page.routes.empty() || page.routes.size() > RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES) {
				throwRouteError(FormatError::FERR_INVALID_HEADER, "metadata PageId route leaf has an invalid header");
			}
			uint64_t previous = 0U;
			for (const MetadataPageRoute &route: page.routes) {
				if (!route.page_id.isValid() || route.page_id.value() <= previous) {
					throwRouteError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route leaf routes are not strictly ordered");
				}
				validateReference(route.physical_reference);
				previous = route.page_id.value();
			}
		}

		void writeHeader(std::span<std::byte> bytes, const MetadataPageRouteLeafPage &page, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_METADATA_PAGE_ROUTE_LEAF_MAGIC);
			LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_METADATA_PAGE_ROUTE_LEAF_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, page.page_id.value());
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, page.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, page.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, ENTRY_COUNT_OFFSET, static_cast<uint32_t>(page.routes.size()));
			LittleEndianCodec::writeU32(bytes, ENTRY_BYTES_OFFSET, RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES);
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU64(bytes, RESERVED_OFFSET, 0U);
			LittleEndianCodec::writeU64(bytes, RESERVED_OFFSET + sizeof(uint64_t), 0U);
		}

	} // namespace

	MetadataPageRouteLeafCodec::Bytes MetadataPageRouteLeafCodec::encode(const MetadataPageRouteLeafPage &page) {
		validatePage(page);
		Bytes bytes{};
		writeHeader(bytes, page, 0U, 0U);
		for (size_t index = 0U; index < page.routes.size(); ++index) {
			const size_t offset = RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES + index * RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES;
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_PAGE_ID_OFFSET, page.routes[index].page_id.value());
			writeReference(bytes, offset, page.routes[index].physical_reference);
		}
		const uint32_t payloadCrc =
				calculateCrc(std::span<const std::byte>(bytes).subspan(RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES));
		writeHeader(bytes, page, payloadCrc, 0U);
		const uint32_t headerCrc =
				calculateCrc(std::span<const std::byte>(bytes).first(RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES));
		writeHeader(bytes, page, payloadCrc, headerCrc);
		return bytes;
	}

	MetadataPageRouteLeafPage MetadataPageRouteLeafCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwRouteError(FormatError::FERR_BUFFER_TOO_SMALL, "metadata PageId route leaf is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwRouteError(FormatError::FERR_INVALID_HEADER, "metadata PageId route leaf has an unexpected size");
		}
		std::array<std::byte, RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwRouteError(FormatError::FERR_CHECKSUM_MISMATCH,
								"metadata PageId route leaf header checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_METADATA_PAGE_ROUTE_LEAF_MAGIC) {
			throwRouteError(FormatError::FERR_INVALID_MAGIC, "metadata PageId route leaf magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_METADATA_PAGE_ROUTE_LEAF_VERSION) {
			throwRouteError(FormatError::FERR_UNSUPPORTED_VERSION,
								"unsupported metadata PageId route leaf version");
		}
		const uint32_t count = LittleEndianCodec::readU32(bytes, ENTRY_COUNT_OFFSET);
		if (LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, ENTRY_BYTES_OFFSET) != RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES || count == 0U ||
			count > RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES ||
			LittleEndianCodec::readU64(bytes, RESERVED_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED_OFFSET + sizeof(uint64_t)) != 0U) {
			throwRouteError(FormatError::FERR_RESERVED_BITS, "metadata PageId route leaf uses reserved fields");
		}
		if (calculateCrc(bytes.subspan(RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwRouteError(FormatError::FERR_CHECKSUM_MISMATCH,
								"metadata PageId route leaf payload checksum does not match");
		}

		MetadataPageRouteLeafPage page;
		page.page_id = MetadataPageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		page.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		page.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		page.routes.reserve(count);
		for (uint32_t index = 0U; index < count; ++index) {
			const size_t offset = RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES + index * RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES;
			page.routes.push_back({MetadataPageId(LittleEndianCodec::readU64(bytes, offset + ENTRY_PAGE_ID_OFFSET)),
									   readReference(bytes, offset)});
		}
		const size_t usedBytes = RSS_METADATA_PAGE_ROUTE_LEAF_HEADER_BYTES + count * RSS_METADATA_PAGE_ROUTE_ENTRY_BYTES;
		if (!std::all_of(bytes.begin() + usedBytes, bytes.end(), [](std::byte value) { return value == std::byte{}; })) {
			throwRouteError(FormatError::FERR_RESERVED_BITS,
								"metadata PageId route leaf has nonzero reserved payload bytes");
		}
		validatePage(page);
		return page;
	}

} // namespace graph::storage
