/**
 * @file CatalogPage.cpp
 * @brief Explicit fixed-size codec for a relocatable Catalog leaf metadata page.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogPage.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t ENTRY_BYTES_OFFSET = 8;
		constexpr size_t ENTRY_COUNT_OFFSET = 12;
		constexpr size_t PAGE_ID_OFFSET = 16;
		constexpr size_t MINIMUM_SEGMENT_ID_OFFSET = 24;
		constexpr size_t MAXIMUM_SEGMENT_ID_OFFSET = 32;
		constexpr size_t GENERATION_OFFSET = 40;
		constexpr size_t COMMITTED_LSN_OFFSET = 48;
		constexpr size_t PAYLOAD_CRC_OFFSET = 56;
		constexpr size_t HEADER_CRC_OFFSET = 60;

		constexpr size_t ENTRY_SEGMENT_ID_OFFSET = 0;
		constexpr size_t ENTRY_PHYSICAL_SLOT_OFFSET = 8;
		constexpr size_t ENTRY_OWNER_ID_OFFSET = 16;
		constexpr size_t ENTRY_GENERATION_OFFSET = 24;
		constexpr size_t ENTRY_COMMITTED_LSN_OFFSET = 32;
		constexpr size_t ENTRY_KIND_OFFSET = 40;
		constexpr size_t ENTRY_STATE_OFFSET = 42;
		constexpr size_t ENTRY_RESERVED_OFFSET = 44;
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[noreturn]] void throwCatalogPageError(FormatError error, const std::string &message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isKnownSegmentKind(uint16_t rawKind) {
			return rawKind >= static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL) &&
				   rawKind <= static_cast<uint16_t>(SegmentKind::SGK_BULK_SCRATCH);
		}

		[[nodiscard]] bool isKnownSegmentState(uint16_t rawState) {
			return rawState <= static_cast<uint16_t>(SegmentState::SGS_OBSOLETE);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void writeHeader(std::span<std::byte> bytes, const CatalogLeafPage &page, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_CATALOG_PAGE_MAGIC);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, RSS_CATALOG_PAGE_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_CATALOG_PAGE_HEADER_BYTES);
			LittleEndianCodec::writeU32(bytes, ENTRY_BYTES_OFFSET, RSS_CATALOG_ENTRY_BYTES);
			LittleEndianCodec::writeU32(bytes, ENTRY_COUNT_OFFSET, static_cast<uint32_t>(page.entries.size()));
			LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, page.page_id.value());
			LittleEndianCodec::writeU64(bytes, MINIMUM_SEGMENT_ID_OFFSET, page.minimum_segment_id.value());
			LittleEndianCodec::writeU64(bytes, MAXIMUM_SEGMENT_ID_OFFSET, page.maximum_segment_id.value());
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, page.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, page.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
		}

		void writeEntry(std::span<std::byte> bytes, size_t offset, const CatalogEntry &entry) {
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_SEGMENT_ID_OFFSET, entry.segment_id.value());
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_PHYSICAL_SLOT_OFFSET, entry.physical_slot.value());
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_OWNER_ID_OFFSET, entry.owner_id);
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_GENERATION_OFFSET, entry.generation.value());
			LittleEndianCodec::writeU64(bytes, offset + ENTRY_COMMITTED_LSN_OFFSET, entry.committed_lsn.value());
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_KIND_OFFSET, static_cast<uint16_t>(entry.kind));
			LittleEndianCodec::writeU16(bytes, offset + ENTRY_STATE_OFFSET, static_cast<uint16_t>(entry.state));
			LittleEndianCodec::writeU32(bytes, offset + ENTRY_RESERVED_OFFSET, 0);
		}

		[[nodiscard]] CatalogEntry readEntry(std::span<const std::byte> bytes, size_t offset) {
			CatalogEntry entry;
			entry.segment_id = SegmentId(LittleEndianCodec::readU64(bytes, offset + ENTRY_SEGMENT_ID_OFFSET));
			entry.physical_slot =
					PhysicalSlotId(LittleEndianCodec::readU64(bytes, offset + ENTRY_PHYSICAL_SLOT_OFFSET));
			entry.owner_id = LittleEndianCodec::readU64(bytes, offset + ENTRY_OWNER_ID_OFFSET);
			entry.generation = Generation(LittleEndianCodec::readU64(bytes, offset + ENTRY_GENERATION_OFFSET));
			entry.committed_lsn =
					LogSequenceNumber(LittleEndianCodec::readU64(bytes, offset + ENTRY_COMMITTED_LSN_OFFSET));
			entry.kind = static_cast<SegmentKind>(LittleEndianCodec::readU16(bytes, offset + ENTRY_KIND_OFFSET));
			entry.state = static_cast<SegmentState>(LittleEndianCodec::readU16(bytes, offset + ENTRY_STATE_OFFSET));
			if (LittleEndianCodec::readU32(bytes, offset + ENTRY_RESERVED_OFFSET) != 0) {
				throwCatalogPageError(FormatError::FERR_RESERVED_BITS, "relocatable Catalog entry uses reserved bytes");
			}
			return entry;
		}

		void validateEntry(const CatalogEntry &entry, LogSequenceNumber pageLsn) {
			if (!isKnownSegmentKind(static_cast<uint16_t>(entry.kind))) {
				throwCatalogPageError(FormatError::FERR_INVALID_ENUM,
									  "relocatable Catalog entry has an invalid segment kind " +
											  std::to_string(static_cast<uint16_t>(entry.kind)));
			}
			if (!isKnownSegmentState(static_cast<uint16_t>(entry.state))) {
				throwCatalogPageError(FormatError::FERR_INVALID_ENUM,
									  "relocatable Catalog entry has an invalid segment state");
			}
			if (entry.state != SegmentState::SGS_SEALED && entry.state != SegmentState::SGS_RETIRED &&
				entry.state != SegmentState::SGS_OBSOLETE) {
				throwCatalogPageError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog leaf can persist only sealed, retired, or obsolete entries");
			}
			if (!entry.segment_id.isValid() || !entry.physical_slot.isValid() || !entry.generation.isValid() ||
				!entry.committed_lsn.isValid()) {
				throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog entry has an invalid identifier");
			}
			if (entry.committed_lsn > pageLsn) {
				throwCatalogPageError(FormatError::FERR_INVALID_STATE,
									  "relocatable Catalog entry is newer than its committed page");
			}
		}

		void validatePage(const CatalogLeafPage &page) {
			if (!page.page_id.isValid() || !page.generation.isValid() || !page.committed_lsn.isValid() ||
				page.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION ||
				page.entries.size() > RSS_CATALOG_PAGE_MAX_ENTRIES) {
				throwCatalogPageError(FormatError::FERR_INVALID_HEADER, "invalid relocatable Catalog leaf header");
			}
			if (page.entries.empty()) {
				if (page.minimum_segment_id.isValid() || page.maximum_segment_id.isValid()) {
					throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
										  "empty relocatable Catalog leaf must not claim a segment range");
				}
				return;
			}
			if (!page.minimum_segment_id.isValid() || !page.maximum_segment_id.isValid()) {
				throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
									  "nonempty relocatable Catalog leaf needs a segment range");
			}

			std::unordered_map<uint64_t, SegmentState> physicalSlots;
			physicalSlots.reserve(page.entries.size());
			auto claimSlot = [&](PhysicalSlotId slot, SegmentState state) {
				auto [position, inserted] = physicalSlots.emplace(slot.value(), state);
				if (!inserted && position->second == SegmentState::SGS_SEALED && state == SegmentState::SGS_SEALED) {
					throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
										  "relocatable Catalog leaf maps multiple sealed entries to one physical slot");
				}
				if (state == SegmentState::SGS_SEALED) {
					position->second = SegmentState::SGS_SEALED;
				}
			};
			SegmentId previousSegmentId;
			for (const CatalogEntry &entry: page.entries) {
				validateEntry(entry, page.committed_lsn);
				if (previousSegmentId.isValid() && entry.segment_id <= previousSegmentId) {
					throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
										  "relocatable Catalog entries must have strictly increasing segment IDs");
				}
				claimSlot(entry.physical_slot, entry.state);
				previousSegmentId = entry.segment_id;
			}
			if (page.minimum_segment_id != page.entries.front().segment_id ||
				page.maximum_segment_id != page.entries.back().segment_id) {
				throwCatalogPageError(FormatError::FERR_INVALID_HEADER,
									  "relocatable Catalog leaf range does not match its entries");
			}
		}

		void validateHeader(uint32_t magic, uint16_t version, uint16_t headerBytes, uint32_t entryBytes,
							uint32_t entryCount) {
			if (magic != RSS_CATALOG_PAGE_MAGIC) {
				throwCatalogPageError(FormatError::FERR_INVALID_MAGIC, "relocatable Catalog page magic does not match");
			}
			if (version != RSS_CATALOG_PAGE_VERSION) {
				throwCatalogPageError(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported relocatable Catalog page version");
			}
			if (headerBytes != RSS_CATALOG_PAGE_HEADER_BYTES || entryBytes != RSS_CATALOG_ENTRY_BYTES ||
				entryCount > RSS_CATALOG_PAGE_MAX_ENTRIES) {
				throwCatalogPageError(FormatError::FERR_INVALID_HEADER, "invalid relocatable Catalog page dimensions");
			}
		}

	} // namespace

	std::array<std::byte, RSS_PAGE_SIZE> CatalogLeafPageCodec::encode(const CatalogLeafPage &page) {
		validatePage(page);
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		writeHeader(bytes, page, 0, 0);
		for (size_t index = 0; index < page.entries.size(); ++index) {
			writeEntry(bytes, RSS_CATALOG_PAGE_HEADER_BYTES + index * RSS_CATALOG_ENTRY_BYTES, page.entries[index]);
		}
		const uint32_t payloadCrc = calculateCrc(std::span(bytes).subspan(RSS_CATALOG_PAGE_HEADER_BYTES));
		writeHeader(bytes, page, payloadCrc, 0);
		const uint32_t headerCrc = calculateCrc(std::span(bytes).first(RSS_CATALOG_PAGE_HEADER_BYTES));
		writeHeader(bytes, page, payloadCrc, headerCrc);
		return bytes;
	}

	CatalogLeafPage CatalogLeafPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwCatalogPageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Catalog page is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwCatalogPageError(FormatError::FERR_INVALID_HEADER, "relocatable Catalog page has an unexpected size");
		}

		std::array<std::byte, RSS_CATALOG_PAGE_HEADER_BYTES> headerBytes{};
		std::copy_n(bytes.begin(), headerBytes.size(), headerBytes.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(headerBytes, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(headerBytes, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(headerBytes) != expectedHeaderCrc) {
			throwCatalogPageError(FormatError::FERR_CHECKSUM_MISMATCH,
								  "relocatable Catalog page header checksum does not match");
		}

		const uint32_t magic = LittleEndianCodec::readU32(bytes, MAGIC_OFFSET);
		const uint16_t version = LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET);
		const uint16_t headerSize = LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET);
		const uint32_t entrySize = LittleEndianCodec::readU32(bytes, ENTRY_BYTES_OFFSET);
		const uint32_t entryCount = LittleEndianCodec::readU32(bytes, ENTRY_COUNT_OFFSET);
		validateHeader(magic, version, headerSize, entrySize, entryCount);

		const std::span<const std::byte> payload = bytes.subspan(RSS_CATALOG_PAGE_HEADER_BYTES);
		if (calculateCrc(payload) != LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwCatalogPageError(FormatError::FERR_CHECKSUM_MISMATCH,
								  "relocatable Catalog page payload checksum does not match");
		}
		const size_t usedPayloadBytes = static_cast<size_t>(entryCount) * RSS_CATALOG_ENTRY_BYTES;
		if (std::ranges::any_of(payload.subspan(usedPayloadBytes), [](std::byte value) {
				return value != std::byte{};
			})) {
			throwCatalogPageError(FormatError::FERR_RESERVED_BITS,
								  "relocatable Catalog page has nonzero unused payload bytes");
		}

		CatalogLeafPage page;
		page.page_id = MetadataPageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		page.minimum_segment_id = SegmentId(LittleEndianCodec::readU64(bytes, MINIMUM_SEGMENT_ID_OFFSET));
		page.maximum_segment_id = SegmentId(LittleEndianCodec::readU64(bytes, MAXIMUM_SEGMENT_ID_OFFSET));
		page.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		page.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		page.entries.reserve(entryCount);
		for (uint32_t index = 0; index < entryCount; ++index) {
			CatalogEntry entry = readEntry(bytes, RSS_CATALOG_PAGE_HEADER_BYTES + index * RSS_CATALOG_ENTRY_BYTES);
			if (!isKnownSegmentKind(static_cast<uint16_t>(entry.kind))) {
				throwCatalogPageError(FormatError::FERR_INVALID_ENUM,
									  "relocatable Catalog decoder read an invalid segment kind " +
											  std::to_string(static_cast<uint16_t>(entry.kind)));
			}
			page.entries.push_back(std::move(entry));
		}
		validatePage(page);
		return page;
	}

} // namespace graph::storage
