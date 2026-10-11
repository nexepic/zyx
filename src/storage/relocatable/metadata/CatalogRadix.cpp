/**
 * @file CatalogRadix.cpp
 * @brief Explicit codec for relocatable Catalog radix root and internal pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t KIND_OFFSET = 8;
		constexpr size_t LEVEL_OFFSET = 10;
		constexpr size_t FLAGS_OFFSET = 12;
		constexpr size_t CHILD_BYTES_OFFSET = 16;
		constexpr size_t CHILD_COUNT_OFFSET = 20;
		constexpr size_t PAGE_ID_OFFSET = 24;
		constexpr size_t GENERATION_OFFSET = 32;
		constexpr size_t COMMITTED_LSN_OFFSET = 40;
		constexpr size_t PAYLOAD_CRC_OFFSET = 48;
		constexpr size_t HEADER_CRC_OFFSET = 52;
		constexpr size_t RESERVED_OFFSET = 56;
		constexpr size_t ALLOCATOR_FREE_CHILDREN_OFFSET = 64;
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[noreturn]] void throwCatalogRadixError(FormatError error, const std::string &message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isKnownKind(uint16_t rawKind) {
			return rawKind == static_cast<uint16_t>(CatalogRadixPageKind::CRPK_ROOT) ||
				   rawKind == static_cast<uint16_t>(CatalogRadixPageKind::CRPK_INTERNAL);
		}

		[[nodiscard]] bool isKnownDirectoryType(uint32_t rawType) {
			return rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_CATALOG) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_ALLOCATOR) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_PAGE) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_EXTENT) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_NODE_ENTITY) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_EDGE_ENTITY) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_INDEX_ROOT) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_VECTOR_LOCATION) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_VECTOR_MANIFEST) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_STATE) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_RETIRE_QUEUE) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_ENTITY_PROPERTY) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST) ||
				   rawType == static_cast<uint32_t>(MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE);
		}

		void writeChild(std::span<std::byte> bytes, size_t offset, const PhysicalPageRef &child) {
			LittleEndianCodec::writeU64(bytes, offset, child.physical_slot.value());
			if (!child.physical_slot.isValid()) {
				LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t), 0);
				return;
			}
			LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t),
										(child.generation.value() << 5U) | child.frame_index);
		}

		[[nodiscard]] PhysicalPageRef readChild(std::span<const std::byte> bytes, size_t offset) {
			PhysicalPageRef child;
			child.physical_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, offset));
			const uint64_t packed = LittleEndianCodec::readU64(bytes, offset + sizeof(uint64_t));
			if (!child.physical_slot.isValid()) {
				if (packed != 0) {
					throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
										   "an absent relocatable Catalog radix child has nonzero location data");
				}
				return child;
			}
			child.frame_index = static_cast<uint8_t>(packed & 0x1FU);
			child.generation = Generation(packed >> 5U);
			return child;
		}

		void validateChild(const PhysicalPageRef &child) {
			const bool hasSlot = child.physical_slot.isValid();
			const bool hasLocation = child.frame_index != 0 || child.generation.isValid();
			if (!hasSlot && !hasLocation) {
				return;
			}
			if (!child.isValid() || child.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "invalid relocatable Catalog radix physical child reference");
			}
		}

		[[nodiscard]] uint32_t countChildren(const CatalogRadixPage &page) {
			uint32_t count = 0;
			for (const PhysicalPageRef &child: page.children) {
				validateChild(child);
				count += child.isValid() ? 1U : 0U;
			}
			return count;
		}

		[[nodiscard]] uint32_t countLogicalChildren(const CatalogRadixPage &page) {
			uint32_t count = 0;
			for (const MetadataPageId child: page.logical_children) {
				count += child.isValid() ? 1U : 0U;
			}
			return count;
		}

		[[nodiscard]] bool hasOnlyZeroAllocatorSummary(const CatalogRadixPage &page) {
			return std::all_of(page.allocator_free_children.begin(), page.allocator_free_children.end(),
							   [](std::byte value) { return value == std::byte{}; });
		}

		void validateAllocatorSummary(const CatalogRadixPage &page) {
			if (page.directory_type != MetadataDirectoryType::MDT_ALLOCATOR) {
				if (!hasOnlyZeroAllocatorSummary(page)) {
					throwCatalogRadixError(FormatError::FERR_RESERVED_BITS,
										   "non-allocator metadata radix page has an allocator free-space summary");
				}
				return;
			}

			const uint32_t remainderBits = RSS_CATALOG_RADIX_FANOUT % 8U;
			if (remainderBits == 0U) {
				return;
			}
			const uint32_t usedBytes = (RSS_CATALOG_RADIX_FANOUT + 7U) / 8U;
			const uint8_t unusedMask = static_cast<uint8_t>(~((1U << remainderBits) - 1U));
			if ((std::to_integer<uint8_t>(page.allocator_free_children[usedBytes - 1U]) & unusedMask) != 0U) {
				throwCatalogRadixError(
						FormatError::FERR_RESERVED_BITS,
						"allocator metadata radix page sets free-space summary bits outside its fan-out");
			}
			for (uint32_t index = usedBytes; index < RSS_CATALOG_RADIX_ALLOCATOR_SUMMARY_BYTES; ++index) {
				if (page.allocator_free_children[index] != std::byte{}) {
					throwCatalogRadixError(FormatError::FERR_RESERVED_BITS,
										   "allocator metadata radix page has nonzero unused free-space summary bytes");
				}
			}
		}

		void validatePage(const CatalogRadixPage &page) {
			if (!isKnownKind(static_cast<uint16_t>(page.kind))) {
				throwCatalogRadixError(FormatError::FERR_INVALID_ENUM, "invalid relocatable Catalog radix page kind");
			}
			if (!isKnownDirectoryType(static_cast<uint32_t>(page.directory_type))) {
				throwCatalogRadixError(FormatError::FERR_INVALID_ENUM, "invalid relocatable metadata directory type");
			}
			// Level zero is the leaf-parent level: its children are Catalog leaves.
			if (page.level > RSS_CATALOG_RADIX_MAX_LEVEL || !page.page_id.isValid() || !page.generation.isValid() ||
				!page.committed_lsn.isValid()) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "invalid relocatable Catalog radix page header");
			}
			validateAllocatorSummary(page);
			if (page.logical_children_enabled) {
				if (page.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
					throwCatalogRadixError(FormatError::FERR_INVALID_STATE,
										   "metadata PageId route branch must retain physical children");
				}
				for (size_t index = 0U; index < page.children.size(); ++index) {
					const PhysicalPageRef &child = page.children[index];
					const bool hasPhysicalData = child.isValid() || child.physical_slot.isValid() ||
												 child.frame_index != 0 || child.generation.isValid();
					const bool resolvedChild =
							page.logical_children_resolved && page.logical_children[index].isValid() && child.isValid();
					if (hasPhysicalData && !resolvedChild) {
						throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
											   "logical metadata radix page has physical child data");
					}
				}
				static_cast<void>(countLogicalChildren(page));
				return;
			}
			for (const MetadataPageId child: page.logical_children) {
				if (child.isValid()) {
					throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
										   "physical metadata radix page has logical child data");
				}
			}
			if (page.logical_children_resolved) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "physical metadata radix page cannot retain logical resolution state");
			}
			static_cast<void>(countChildren(page));
		}

		void writeHeader(std::span<std::byte> bytes, const CatalogRadixPage &page, uint32_t childCount,
						 uint32_t payloadCrc, uint32_t headerCrc, uint16_t version) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_CATALOG_RADIX_PAGE_MAGIC);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, version);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_CATALOG_RADIX_HEADER_BYTES);
			LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(page.kind));
			LittleEndianCodec::writeU16(bytes, LEVEL_OFFSET, page.level);
			LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, static_cast<uint32_t>(page.directory_type));
			LittleEndianCodec::writeU32(bytes, CHILD_BYTES_OFFSET, RSS_CATALOG_RADIX_CHILD_BYTES);
			LittleEndianCodec::writeU32(bytes, CHILD_COUNT_OFFSET, childCount);
			LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, page.page_id.value());
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, page.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, page.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU64(bytes, RESERVED_OFFSET, 0);
			std::copy(page.allocator_free_children.begin(), page.allocator_free_children.end(),
					  bytes.begin() + ALLOCATOR_FREE_CHILDREN_OFFSET);
		}

		void validateHeader(uint32_t magic, uint16_t version, uint16_t headerBytes, uint16_t rawKind, uint16_t level,
							uint32_t flags, uint32_t childBytes, uint32_t childCount, uint64_t reserved) {
			if (magic != RSS_CATALOG_RADIX_PAGE_MAGIC) {
				throwCatalogRadixError(FormatError::FERR_INVALID_MAGIC,
									   "relocatable Catalog radix page magic does not match");
			}
			if (version != RSS_CATALOG_RADIX_PAGE_VERSION && version != RSS_CATALOG_RADIX_LOGICAL_PAGE_VERSION) {
				throwCatalogRadixError(FormatError::FERR_UNSUPPORTED_VERSION,
									   "unsupported relocatable Catalog radix page version");
			}
			if (!isKnownKind(rawKind)) {
				throwCatalogRadixError(FormatError::FERR_INVALID_ENUM, "invalid relocatable Catalog radix page kind");
			}
			if (headerBytes != RSS_CATALOG_RADIX_HEADER_BYTES || childBytes != RSS_CATALOG_RADIX_CHILD_BYTES ||
				childCount > RSS_CATALOG_RADIX_FANOUT || level > RSS_CATALOG_RADIX_MAX_LEVEL) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "invalid relocatable Catalog radix page dimensions");
			}
			const uint32_t rawDirectoryType = flags & RSS_METADATA_DIRECTORY_TYPE_MASK;
			const uint8_t invalidFlags = static_cast<uint8_t>((flags & ~RSS_METADATA_DIRECTORY_TYPE_MASK) != 0U) |
										 static_cast<uint8_t>(!isKnownDirectoryType(rawDirectoryType)) |
										 static_cast<uint8_t>(reserved != 0U);
			if (invalidFlags != 0U) {
				throwCatalogRadixError(FormatError::FERR_RESERVED_BITS,
									   "relocatable Catalog radix page uses reserved bits");
			}
		}

		void validateRootCoverage(const CatalogRadixPage &page, SegmentId segmentId) {
			if (page.kind != CatalogRadixPageKind::CRPK_ROOT) {
				return;
			}
			uint64_t quotient = (segmentId.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES;
			for (uint16_t index = 0; index <= page.level; ++index) {
				quotient /= RSS_CATALOG_RADIX_FANOUT;
			}
			if (quotient != 0) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "relocatable Catalog radix root does not cover the requested segment ID");
			}
		}

	} // namespace

	std::array<std::byte, RSS_PAGE_SIZE> CatalogRadixPageCodec::encode(const CatalogRadixPage &page) {
		validatePage(page);
		const uint32_t childCount = page.logical_children_enabled ? countLogicalChildren(page) : countChildren(page);
		const uint16_t version =
				page.logical_children_enabled ? RSS_CATALOG_RADIX_LOGICAL_PAGE_VERSION : RSS_CATALOG_RADIX_PAGE_VERSION;
		std::array<std::byte, RSS_PAGE_SIZE> bytes{};
		writeHeader(bytes, page, childCount, 0, 0, version);
		for (size_t index = 0; index < page.children.size(); ++index) {
			const size_t offset = RSS_CATALOG_RADIX_HEADER_BYTES + index * RSS_CATALOG_RADIX_CHILD_BYTES;
			if (page.logical_children_enabled) {
				LittleEndianCodec::writeU64(bytes, offset, page.logical_children[index].value());
				LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t), 0);
			} else {
				writeChild(bytes, offset, page.children[index]);
			}
		}
		const uint32_t payloadCrc = calculateCrc(std::span(bytes).subspan(RSS_CATALOG_RADIX_HEADER_BYTES));
		writeHeader(bytes, page, childCount, payloadCrc, 0, version);
		const uint32_t headerCrc = calculateCrc(std::span(bytes).first(RSS_CATALOG_RADIX_HEADER_BYTES));
		writeHeader(bytes, page, childCount, payloadCrc, headerCrc, version);
		return bytes;
	}

	CatalogRadixPage CatalogRadixPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwCatalogRadixError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Catalog radix page is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Catalog radix page has an unexpected size");
		}

		std::array<std::byte, RSS_CATALOG_RADIX_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwCatalogRadixError(FormatError::FERR_CHECKSUM_MISMATCH,
								   "relocatable Catalog radix header checksum does not match");
		}

		const uint32_t childCount = LittleEndianCodec::readU32(bytes, CHILD_COUNT_OFFSET);
		const uint16_t version = LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET);
		validateHeader(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET), version,
					   LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET),
					   LittleEndianCodec::readU16(bytes, KIND_OFFSET), LittleEndianCodec::readU16(bytes, LEVEL_OFFSET),
					   LittleEndianCodec::readU32(bytes, FLAGS_OFFSET),
					   LittleEndianCodec::readU32(bytes, CHILD_BYTES_OFFSET), childCount,
					   LittleEndianCodec::readU64(bytes, RESERVED_OFFSET));
		if (calculateCrc(bytes.subspan(RSS_CATALOG_RADIX_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwCatalogRadixError(FormatError::FERR_CHECKSUM_MISMATCH,
								   "relocatable Catalog radix payload checksum does not match");
		}

		CatalogRadixPage page;
		page.kind = static_cast<CatalogRadixPageKind>(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		page.directory_type = static_cast<MetadataDirectoryType>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) &
																 RSS_METADATA_DIRECTORY_TYPE_MASK);
		page.level = LittleEndianCodec::readU16(bytes, LEVEL_OFFSET);
		page.page_id = MetadataPageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		page.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		page.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		page.logical_children_enabled = version == RSS_CATALOG_RADIX_LOGICAL_PAGE_VERSION;
		page.logical_children_resolved = false;
		std::copy_n(bytes.begin() + ALLOCATOR_FREE_CHILDREN_OFFSET, page.allocator_free_children.size(),
					page.allocator_free_children.begin());
		for (size_t index = 0; index < page.children.size(); ++index) {
			const size_t offset = RSS_CATALOG_RADIX_HEADER_BYTES + index * RSS_CATALOG_RADIX_CHILD_BYTES;
			if (page.logical_children_enabled) {
				const uint64_t logicalId = LittleEndianCodec::readU64(bytes, offset);
				const uint64_t reserved = LittleEndianCodec::readU64(bytes, offset + sizeof(uint64_t));
				if (reserved != 0U) {
					throwCatalogRadixError(FormatError::FERR_RESERVED_BITS,
										   "logical metadata radix child has nonzero reserved location data");
				}
				page.logical_children[index] = MetadataPageId(logicalId);
			} else {
				page.children[index] = readChild(bytes, offset);
			}
		}
		validatePage(page);
		const uint32_t decodedChildCount =
				page.logical_children_enabled ? countLogicalChildren(page) : countChildren(page);
		if (decodedChildCount != childCount) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Catalog radix child count does not match its references");
		}
		return page;
	}

	uint32_t CatalogRadixPageCodec::childIndex(SegmentId segmentId, uint16_t level) {
		if (!segmentId.isValid() || level > RSS_CATALOG_RADIX_MAX_LEVEL) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER, "invalid relocatable Catalog radix route");
		}
		return childIndexForLeafOrdinal((segmentId.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES, level);
	}

	uint32_t CatalogRadixPageCodec::childIndexForLeafOrdinal(uint64_t leafOrdinal, uint16_t level) {
		if (level > RSS_CATALOG_RADIX_MAX_LEVEL) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER, "invalid relocatable metadata radix route");
		}
		uint64_t quotient = leafOrdinal;
		for (uint16_t index = 0; index < level; ++index) {
			quotient /= RSS_CATALOG_RADIX_FANOUT;
		}
		return static_cast<uint32_t>(quotient % RSS_CATALOG_RADIX_FANOUT);
	}

	const PhysicalPageRef &CatalogRadixPageCodec::childFor(const CatalogRadixPage &page, SegmentId segmentId) {
		validatePage(page);
		if (page.directory_type != MetadataDirectoryType::MDT_CATALOG) {
			throwCatalogRadixError(FormatError::FERR_INVALID_STATE,
								   "Catalog routing requires a Catalog metadata directory page");
		}
		validateRootCoverage(page, segmentId);
		const uint32_t index = childIndex(segmentId, page.level);
		const PhysicalPageRef &child = page.children[index];
		if (!child.isValid()) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Catalog radix route has no child page");
		}
		return child;
	}

	std::optional<PhysicalPageRef> CatalogRadixPageCodec::findChildFor(const CatalogRadixPage &page,
																	   SegmentId segmentId) {
		validatePage(page);
		if (page.directory_type != MetadataDirectoryType::MDT_CATALOG) {
			throwCatalogRadixError(FormatError::FERR_INVALID_STATE,
								   "Catalog routing requires a Catalog metadata directory page");
		}
		if (!segmentId.isValid()) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER, "invalid relocatable Catalog radix route");
		}
		return findChildForLeafOrdinal(page, (segmentId.value() - 1U) / RSS_CATALOG_PAGE_MAX_ENTRIES);
	}

	std::optional<PhysicalPageRef> CatalogRadixPageCodec::findChildForLeafOrdinal(const CatalogRadixPage &page,
																				  uint64_t leafOrdinal) {
		validatePage(page);
		if (page.kind == CatalogRadixPageKind::CRPK_ROOT) {
			uint64_t quotient = leafOrdinal;
			for (uint16_t index = 0; index <= page.level; ++index) {
				quotient /= RSS_CATALOG_RADIX_FANOUT;
			}
			if (quotient != 0U) {
				return std::nullopt;
			}
		}
		const PhysicalPageRef &child = page.children[childIndexForLeafOrdinal(leafOrdinal, page.level)];
		return child.isValid() ? std::optional<PhysicalPageRef>(child) : std::nullopt;
	}

	const PhysicalPageRef &CatalogRadixPageCodec::childForLeafOrdinal(const CatalogRadixPage &page,
																	  uint64_t leafOrdinal) {
		validatePage(page);
		if (page.kind == CatalogRadixPageKind::CRPK_ROOT) {
			uint64_t quotient = leafOrdinal;
			for (uint16_t index = 0; index <= page.level; ++index) {
				quotient /= RSS_CATALOG_RADIX_FANOUT;
			}
			if (quotient != 0U) {
				throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
									   "relocatable metadata radix root does not cover the requested leaf ordinal");
			}
		}
		const PhysicalPageRef &child = page.children[childIndexForLeafOrdinal(leafOrdinal, page.level)];
		if (!child.isValid()) {
			throwCatalogRadixError(FormatError::FERR_INVALID_HEADER,
								   "relocatable metadata radix route has no child page");
		}
		return child;
	}

} // namespace graph::storage
