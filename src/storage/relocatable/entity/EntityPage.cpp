/**
 * @file EntityPage.cpp
 * @brief Fixed Node/Edge page codec for the native relocatable store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/EntityPage.hpp"

#include <algorithm>
#include <limits>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t PAGE_MAGIC_OFFSET = 0;
		constexpr size_t PAGE_VERSION_OFFSET = 4;
		constexpr size_t PAGE_HEADER_BYTES_OFFSET = 6;
		constexpr size_t PAGE_RECORD_KIND_OFFSET = 8;
		constexpr size_t PAGE_FLAGS_OFFSET = 10;
		constexpr size_t PAGE_RECORD_BYTES_OFFSET = 12;
		constexpr size_t PAGE_SLOT_CAPACITY_OFFSET = 16;
		constexpr size_t PAGE_LIVE_COUNT_OFFSET = 18;
		constexpr size_t PAGE_BITMAP_BYTES_OFFSET = 20;
		constexpr size_t PAGE_RESERVED0_OFFSET = 22;
		constexpr size_t PAGE_ID_OFFSET = 24;
		constexpr size_t PAGE_GENERATION_OFFSET = 32;
		constexpr size_t PAGE_PAYLOAD_CRC_OFFSET = 40;
		constexpr size_t PAGE_HEADER_CRC_OFFSET = 44;
		constexpr size_t PAGE_COMMITTED_LSN_OFFSET = 48;
		constexpr size_t PAGE_RESERVED1_OFFSET = 56;

		constexpr size_t NODE_LABELS_OFFSET = 0;
		constexpr size_t NODE_LABEL_OVERFLOW_PAGE_OFFSET = 24;
		constexpr size_t NODE_LABEL_OVERFLOW_SLOT_OFFSET = 32;
		constexpr size_t NODE_RESERVED0_OFFSET = 34;
		constexpr size_t NODE_LABEL_OVERFLOW_GENERATION_OFFSET = 36;
		constexpr size_t NODE_INLINE_LABEL_COUNT_OFFSET = 40;
		constexpr size_t NODE_RESERVED1_OFFSET = 41;

		constexpr size_t EDGE_SOURCE_OFFSET = 0;
		constexpr size_t EDGE_TARGET_OFFSET = 8;
		constexpr size_t EDGE_TYPE_OFFSET = 16;

		[[noreturn]] void throwEntityPageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isEntityPageRecordKind(EntityPageRecordKind kind) {
			return kind == EntityPageRecordKind::EPRK_NODE || kind == EntityPageRecordKind::EPRK_EDGE;
		}

		[[nodiscard]] uint16_t recordBytesFor(EntityPageRecordKind kind) {
			switch (kind) {
				case EntityPageRecordKind::EPRK_NODE:
					return RSS_ENTITY_NODE_RECORD_BYTES;
				case EntityPageRecordKind::EPRK_EDGE:
					return RSS_ENTITY_EDGE_RECORD_BYTES;
			}
			throwEntityPageError(FormatError::FERR_INVALID_ENUM, "relocatable Entity page has an unknown record kind");
		}

		[[nodiscard]] constexpr uint16_t requiredBitmapBytes(uint16_t capacity) {
			return static_cast<uint16_t>((static_cast<uint32_t>(capacity) + 7U) / 8U);
		}

		template<uint16_t RecordBytes>
		[[nodiscard]] consteval uint16_t solveFixedPageCapacity() {
			uint16_t result = 0U;
			for (uint32_t candidate = 1U; candidate <= std::numeric_limits<uint16_t>::max(); ++candidate) {
				const uint64_t bytes = static_cast<uint64_t>(RSS_ENTITY_PAGE_HEADER_BYTES) +
									   requiredBitmapBytes(static_cast<uint16_t>(candidate)) +
									   candidate * sizeof(uint32_t) + candidate * RecordBytes;
				if (bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
					break;
				}
				result = static_cast<uint16_t>(candidate);
			}
			return result;
		}

		constexpr uint16_t RSS_ENTITY_NODE_PAGE_CAPACITY = solveFixedPageCapacity<RSS_ENTITY_NODE_RECORD_BYTES>();
		constexpr uint16_t RSS_ENTITY_EDGE_PAGE_CAPACITY = solveFixedPageCapacity<RSS_ENTITY_EDGE_RECORD_BYTES>();
		static_assert(RSS_ENTITY_NODE_PAGE_CAPACITY != 0U && RSS_ENTITY_EDGE_PAGE_CAPACITY != 0U);

		[[nodiscard]] uint16_t capacityFor(EntityPageRecordKind kind) {
			switch (kind) {
				case EntityPageRecordKind::EPRK_NODE:
					return RSS_ENTITY_NODE_PAGE_CAPACITY;
				case EntityPageRecordKind::EPRK_EDGE:
					return RSS_ENTITY_EDGE_PAGE_CAPACITY;
			}
			throwEntityPageError(FormatError::FERR_INVALID_ENUM, "relocatable Entity page has an unknown record kind");
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateOverflowReference(const EntityLabelOverflowReference &reference) {
			if (!reference.isEmpty() && !reference.isValid()) {
				throwEntityPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable Node record has a partial label-overflow reference");
			}
		}

		void validateNodeRecord(const NodeEntityRecord &record) {
			if (record.inline_label_count > record.inline_label_ids.size()) {
				throwEntityPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable Node record has too many inline labels");
			}
			uint64_t previous = 0U;
			for (size_t index = 0U; index < record.inline_label_ids.size(); ++index) {
				const uint64_t label = record.inline_label_ids[index];
				if (index < record.inline_label_count) {
					if (label == 0U || (index != 0U && label <= previous)) {
						throwEntityPageError(FormatError::FERR_INVALID_HEADER,
											 "relocatable Node inline labels are not canonical");
					}
					previous = label;
				} else if (label != 0U) {
					throwEntityPageError(FormatError::FERR_RESERVED_BITS,
										 "relocatable Node record has nonzero unused inline labels");
				}
			}
			validateOverflowReference(record.label_overflow);
			if (!record.label_overflow.isEmpty() && record.inline_label_count != RSS_ENTITY_INLINE_LABEL_CAPACITY) {
				throwEntityPageError(
						FormatError::FERR_INVALID_HEADER,
						"relocatable Node record with label overflow must contain the full inline label prefix");
			}
		}

		void validateEdgeRecord(const EdgeEntityRecord &record) {
			if (record.source_node_id == 0U || record.target_node_id == 0U) {
				throwEntityPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable Edge record requires source and target Node IDs");
			}
		}

		void validateHeader(const EntityPageHeader &header) {
			const uint8_t invalid =
					static_cast<uint8_t>(!header.page_id.isValid()) |
					static_cast<uint8_t>(!header.generation.isValid()) |
					static_cast<uint8_t>(!header.committed_lsn.isValid()) |
					static_cast<uint8_t>(!isEntityPageRecordKind(header.record_kind)) |
					static_cast<uint8_t>(header.record_bytes != recordBytesFor(header.record_kind)) |
					static_cast<uint8_t>(header.slot_capacity != capacityFor(header.record_kind)) |
					static_cast<uint8_t>(header.live_count > header.slot_capacity) |
					static_cast<uint8_t>(header.bitmap_bytes != requiredBitmapBytes(header.slot_capacity));
			if (invalid != 0U) {
				throwEntityPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable Entity page header has invalid fixed-record layout metadata");
			}
		}

		void validateOwner(const PageOwnerHeader &owner, const EntityPageHeader &header, bool allowRetired) {
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != header.page_id) |
									static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_ENTITY) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
									static_cast<uint8_t>(!allowRetired && owner.flags != 0U) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(owner.generation != header.generation) |
									static_cast<uint8_t>(owner.committed_lsn != header.committed_lsn);
			if (invalid != 0U) {
				throwEntityPageError(FormatError::FERR_INVALID_STATE,
									 "relocatable Entity page owner disagrees with its page header");
			}
		}

	} // namespace

	NodeEntityRecordCodec::Bytes NodeEntityRecordCodec::encode(const NodeEntityRecord &record) {
		validateNodeRecord(record);
		Bytes bytes{};
		for (size_t index = 0U; index < record.inline_label_ids.size(); ++index) {
			LittleEndianCodec::writeU64(bytes, NODE_LABELS_OFFSET + index * sizeof(uint64_t),
										record.inline_label_ids[index]);
		}
		LittleEndianCodec::writeU64(bytes, NODE_LABEL_OVERFLOW_PAGE_OFFSET, record.label_overflow.page_id.value());
		LittleEndianCodec::writeU16(bytes, NODE_LABEL_OVERFLOW_SLOT_OFFSET, record.label_overflow.slot_id);
		LittleEndianCodec::writeU32(bytes, NODE_LABEL_OVERFLOW_GENERATION_OFFSET,
									record.label_overflow.slot_generation);
		bytes[NODE_INLINE_LABEL_COUNT_OFFSET] = static_cast<std::byte>(record.inline_label_count);
		return bytes;
	}

	NodeEntityRecord NodeEntityRecordCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_ENTITY_NODE_RECORD_BYTES) {
			throwEntityPageError(bytes.size() < RSS_ENTITY_NODE_RECORD_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																			 : FormatError::FERR_INVALID_HEADER,
								 "relocatable Node page record has an invalid byte length");
		}
		const bool reserved = LittleEndianCodec::readU16(bytes, NODE_RESERVED0_OFFSET) != 0U ||
							  std::any_of(bytes.begin() + NODE_RESERVED1_OFFSET, bytes.end(),
										  [](std::byte value) { return value != std::byte{}; });
		if (reserved) {
			throwEntityPageError(FormatError::FERR_RESERVED_BITS, "relocatable Node page record uses reserved bytes");
		}
		NodeEntityRecord record;
		for (size_t index = 0U; index < record.inline_label_ids.size(); ++index) {
			record.inline_label_ids[index] =
					LittleEndianCodec::readU64(bytes, NODE_LABELS_OFFSET + index * sizeof(uint64_t));
		}
		record.label_overflow.page_id = PageId(LittleEndianCodec::readU64(bytes, NODE_LABEL_OVERFLOW_PAGE_OFFSET));
		record.label_overflow.slot_id = LittleEndianCodec::readU16(bytes, NODE_LABEL_OVERFLOW_SLOT_OFFSET);
		record.label_overflow.slot_generation =
				LittleEndianCodec::readU32(bytes, NODE_LABEL_OVERFLOW_GENERATION_OFFSET);
		record.inline_label_count = std::to_integer<uint8_t>(bytes[NODE_INLINE_LABEL_COUNT_OFFSET]);
		validateNodeRecord(record);
		return record;
	}

	EdgeEntityRecordCodec::Bytes EdgeEntityRecordCodec::encode(const EdgeEntityRecord &record) {
		validateEdgeRecord(record);
		Bytes bytes{};
		LittleEndianCodec::writeU64(bytes, EDGE_SOURCE_OFFSET, record.source_node_id);
		LittleEndianCodec::writeU64(bytes, EDGE_TARGET_OFFSET, record.target_node_id);
		LittleEndianCodec::writeU64(bytes, EDGE_TYPE_OFFSET, record.type_id);
		return bytes;
	}

	EdgeEntityRecord EdgeEntityRecordCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_ENTITY_EDGE_RECORD_BYTES) {
			throwEntityPageError(bytes.size() < RSS_ENTITY_EDGE_RECORD_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																			 : FormatError::FERR_INVALID_HEADER,
								 "relocatable Edge page record has an invalid byte length");
		}
		EdgeEntityRecord record;
		record.source_node_id = LittleEndianCodec::readU64(bytes, EDGE_SOURCE_OFFSET);
		record.target_node_id = LittleEndianCodec::readU64(bytes, EDGE_TARGET_OFFSET);
		record.type_id = LittleEndianCodec::readU64(bytes, EDGE_TYPE_OFFSET);
		validateEdgeRecord(record);
		return record;
	}

	EntityPageHeaderCodec::Bytes EntityPageHeaderCodec::encode(const EntityPageHeader &header) {
		validateHeader(header);
		Bytes bytes{};
		LittleEndianCodec::writeU32(bytes, PAGE_MAGIC_OFFSET, RSS_ENTITY_PAGE_MAGIC);
		LittleEndianCodec::writeU16(bytes, PAGE_VERSION_OFFSET, RSS_ENTITY_PAGE_VERSION);
		LittleEndianCodec::writeU16(bytes, PAGE_HEADER_BYTES_OFFSET, RSS_ENTITY_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, PAGE_RECORD_KIND_OFFSET, static_cast<uint16_t>(header.record_kind));
		LittleEndianCodec::writeU16(bytes, PAGE_FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(bytes, PAGE_RECORD_BYTES_OFFSET, header.record_bytes);
		LittleEndianCodec::writeU16(bytes, PAGE_SLOT_CAPACITY_OFFSET, header.slot_capacity);
		LittleEndianCodec::writeU16(bytes, PAGE_LIVE_COUNT_OFFSET, header.live_count);
		LittleEndianCodec::writeU16(bytes, PAGE_BITMAP_BYTES_OFFSET, header.bitmap_bytes);
		LittleEndianCodec::writeU16(bytes, PAGE_RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, header.page_id.value());
		LittleEndianCodec::writeU64(bytes, PAGE_GENERATION_OFFSET, header.generation.value());
		LittleEndianCodec::writeU64(bytes, PAGE_COMMITTED_LSN_OFFSET, header.committed_lsn.value());
		return bytes;
	}

	EntityPageHeader EntityPageHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_ENTITY_PAGE_HEADER_BYTES) {
			throwEntityPageError(bytes.size() < RSS_ENTITY_PAGE_HEADER_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																			 : FormatError::FERR_INVALID_HEADER,
								 "relocatable Entity page header has an invalid byte length");
		}
		const uint8_t invalidFixed =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, PAGE_MAGIC_OFFSET) != RSS_ENTITY_PAGE_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, PAGE_VERSION_OFFSET) !=
									 RSS_ENTITY_PAGE_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, PAGE_HEADER_BYTES_OFFSET) !=
									 RSS_ENTITY_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, PAGE_FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, PAGE_RESERVED0_OFFSET) != 0U) |
				static_cast<uint8_t>(std::any_of(bytes.begin() + PAGE_RESERVED1_OFFSET, bytes.end(),
												 [](std::byte value) { return value != std::byte{}; }));
		if (invalidFixed != 0U) {
			throwEntityPageError(FormatError::FERR_RESERVED_BITS,
								 "relocatable Entity page header uses unknown flags or reserved bytes");
		}
		EntityPageHeader header;
		header.record_kind =
				static_cast<EntityPageRecordKind>(LittleEndianCodec::readU16(bytes, PAGE_RECORD_KIND_OFFSET));
		header.record_bytes = LittleEndianCodec::readU16(bytes, PAGE_RECORD_BYTES_OFFSET);
		header.slot_capacity = LittleEndianCodec::readU16(bytes, PAGE_SLOT_CAPACITY_OFFSET);
		header.live_count = LittleEndianCodec::readU16(bytes, PAGE_LIVE_COUNT_OFFSET);
		header.bitmap_bytes = LittleEndianCodec::readU16(bytes, PAGE_BITMAP_BYTES_OFFSET);
		header.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		header.generation = Generation(LittleEndianCodec::readU64(bytes, PAGE_GENERATION_OFFSET));
		header.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, PAGE_COMMITTED_LSN_OFFSET));
		validateHeader(header);
		return header;
	}

	EntityPage::EntityPage(std::vector<std::byte> bytes, EntityPageHeader header) :
		bytes_(std::move(bytes)), header_(std::move(header)) {}

	EntityPage::EntityPage(EntityPage &&other) noexcept :
		bytes_(std::move(other.bytes_)), header_(std::move(other.header_)) {}

	EntityPage &EntityPage::operator=(EntityPage &&other) noexcept {
		if (this != &other) {
			bytes_ = std::move(other.bytes_);
			header_ = std::move(other.header_);
		}
		return *this;
	}

	EntityPage EntityPage::create(const PageOwnerHeader &owner, EntityPageRecordKind recordKind) {
		EntityPageHeader header;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		header.committed_lsn = owner.committed_lsn;
		header.record_kind = recordKind;
		header.record_bytes = recordBytesFor(recordKind);
		header.slot_capacity = capacityFor(recordKind);
		header.bitmap_bytes = requiredBitmapBytes(header.slot_capacity);
		validateHeader(header);
		validateOwner(owner, header, false);
		EntityPage page(std::vector<std::byte>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES), header);
		page.refreshChecksums();
		return page;
	}

	EntityPage EntityPage::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwEntityPageError(bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																				 : FormatError::FERR_INVALID_HEADER,
								 "relocatable Entity page payload has an invalid byte length");
		}
		std::vector<std::byte> copied(bytes.begin(), bytes.end());
		std::array<std::byte, RSS_ENTITY_PAGE_HEADER_BYTES> encodedHeader{};
		std::copy_n(copied.begin(), encodedHeader.size(), encodedHeader.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(encodedHeader, PAGE_HEADER_CRC_OFFSET);
		const uint32_t expectedPayloadCrc = LittleEndianCodec::readU32(encodedHeader, PAGE_PAYLOAD_CRC_OFFSET);
		LittleEndianCodec::writeU32(encodedHeader, PAGE_HEADER_CRC_OFFSET, 0U);
		if (calculateCrc(encodedHeader) != expectedHeaderCrc ||
			calculateCrc(std::span(copied).subspan(RSS_ENTITY_PAGE_HEADER_BYTES)) != expectedPayloadCrc) {
			throwEntityPageError(FormatError::FERR_CHECKSUM_MISMATCH,
								 "relocatable Entity page checksum does not match its payload");
		}
		EntityPage page(std::move(copied), EntityPageHeaderCodec::decode(encodedHeader));
		validateOwner(owner, page.header_, true);
		page.validateLayout();
		return page;
	}

	EntityPageHeader EntityPage::header() const { return header_; }

	uint16_t EntityPage::recordCapacity(EntityPageRecordKind recordKind) { return capacityFor(recordKind); }

	EntityPage EntityPage::rebindOwner(const PageOwnerHeader &owner) const {
		validateLayout();
		EntityPageHeader rebound = header_;
		rebound.generation = owner.generation;
		rebound.committed_lsn = owner.committed_lsn;
		validateOwner(owner, rebound, false);
		EntityPage page(bytes_, rebound);
		page.refreshChecksums();
		page.validateLayout();
		return page;
	}

	uint16_t EntityPage::insert(const NodeEntityRecord &record) {
		if (header_.record_kind != EntityPageRecordKind::EPRK_NODE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Node records");
		}
		const NodeEntityRecordCodec::Bytes encoded = NodeEntityRecordCodec::encode(record);
		validateLayout();
		for (uint16_t slot = 0U; slot < header_.slot_capacity; ++slot) {
			if (!isLive(slot)) {
				const uint32_t previous = LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
				if (previous == std::numeric_limits<uint32_t>::max()) {
					throwEntityPageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										 "relocatable Entity page slot generation is exhausted");
				}
				std::copy(encoded.begin(), encoded.end(), bytes_.begin() + recordOffset(slot));
				setSlotGeneration(slot, previous + 1U);
				setLive(slot, true);
				++header_.live_count;
				refreshChecksums();
				return slot;
			}
		}
		throwEntityPageError(FormatError::FERR_INVALID_STATE, "relocatable Entity page has no free fixed-record slot");
	}

	uint16_t EntityPage::insert(const EdgeEntityRecord &record) {
		if (header_.record_kind != EntityPageRecordKind::EPRK_EDGE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Edge records");
		}
		const EdgeEntityRecordCodec::Bytes encoded = EdgeEntityRecordCodec::encode(record);
		validateLayout();
		for (uint16_t slot = 0U; slot < header_.slot_capacity; ++slot) {
			if (!isLive(slot)) {
				const uint32_t previous = LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
				if (previous == std::numeric_limits<uint32_t>::max()) {
					throwEntityPageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										 "relocatable Entity page slot generation is exhausted");
				}
				std::copy(encoded.begin(), encoded.end(), bytes_.begin() + recordOffset(slot));
				setSlotGeneration(slot, previous + 1U);
				setLive(slot, true);
				++header_.live_count;
				refreshChecksums();
				return slot;
			}
		}
		throwEntityPageError(FormatError::FERR_INVALID_STATE, "relocatable Entity page has no free fixed-record slot");
	}

	std::vector<uint16_t> EntityPage::insertEdges(std::span<const EdgeEntityRecord> records) {
		if (header_.record_kind != EntityPageRecordKind::EPRK_EDGE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page does not store Edge records");
		}
		validateLayout();
		if (records.empty()) {
			return {};
		}
		if (records.size() > static_cast<size_t>(header_.slot_capacity - header_.live_count)) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page has insufficient fixed-record slots for a batch");
		}

		std::vector<EdgeEntityRecordCodec::Bytes> encoded;
		std::vector<uint16_t> slots;
		encoded.reserve(records.size());
		slots.reserve(records.size());
		for (const EdgeEntityRecord &record: records) {
			encoded.push_back(EdgeEntityRecordCodec::encode(record));
		}
		for (uint16_t slot = 0U; slot < header_.slot_capacity && slots.size() < records.size(); ++slot) {
			if (isLive(slot)) {
				continue;
			}
			if (LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot)) == std::numeric_limits<uint32_t>::max()) {
				throwEntityPageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "relocatable Entity page slot generation is exhausted");
			}
			slots.push_back(slot);
		}
		if (slots.size() != records.size()) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
							 "relocatable Entity page batch lost a preflighted free fixed-record slot");
		}
		for (size_t index = 0U; index < slots.size(); ++index) {
			const uint16_t slot = slots[index];
			const uint32_t previous = LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
			std::copy(encoded[index].begin(), encoded[index].end(), bytes_.begin() + recordOffset(slot));
			setSlotGeneration(slot, previous + 1U);
			setLive(slot, true);
		}
		header_.live_count = static_cast<uint16_t>(header_.live_count + slots.size());
		refreshChecksums();
		validateLayout();
		return slots;
	}

	std::optional<NodeEntityRecord> EntityPage::findNode(uint16_t slot) const {
		if (header_.record_kind != EntityPageRecordKind::EPRK_NODE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Node records");
		}
		validateLayout();
		if (slot >= header_.slot_capacity || !isLive(slot)) {
			return std::nullopt;
		}
		return NodeEntityRecordCodec::decode(std::span(bytes_).subspan(recordOffset(slot), header_.record_bytes));
	}

	std::optional<NodeEntityRecord> EntityPage::findNode(uint16_t slot, uint32_t expectedSlotGeneration) const {
		const std::optional<NodeEntityRecord> record = findNode(slot);
		if (!record.has_value()) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity Map references an unavailable Node slot");
		}
		validateSlotGeneration(slot, expectedSlotGeneration);
		return record;
	}

	std::optional<EdgeEntityRecord> EntityPage::findEdge(uint16_t slot) const {
		if (header_.record_kind != EntityPageRecordKind::EPRK_EDGE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Edge records");
		}
		validateLayout();
		if (slot >= header_.slot_capacity || !isLive(slot)) {
			return std::nullopt;
		}
		return EdgeEntityRecordCodec::decode(std::span(bytes_).subspan(recordOffset(slot), header_.record_bytes));
	}

	std::optional<EdgeEntityRecord> EntityPage::findEdge(uint16_t slot, uint32_t expectedSlotGeneration) const {
		const std::optional<EdgeEntityRecord> record = findEdge(slot);
		if (!record.has_value()) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity Map references an unavailable Edge slot");
		}
		validateSlotGeneration(slot, expectedSlotGeneration);
		return record;
	}

	uint32_t EntityPage::slotGeneration(uint16_t slot) const {
		validateLayout();
		if (slot >= header_.slot_capacity) {
			throwEntityPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable Entity page slot is outside its capacity");
		}
		return LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
	}

	void EntityPage::replace(uint16_t slot, const NodeEntityRecord &record) {
		if (header_.record_kind != EntityPageRecordKind::EPRK_NODE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Node records");
		}
		validateLayout();
		validateLiveSlot(slot);
		const NodeEntityRecordCodec::Bytes encoded = NodeEntityRecordCodec::encode(record);
		std::copy(encoded.begin(), encoded.end(), bytes_.begin() + recordOffset(slot));
		refreshChecksums();
	}

	void EntityPage::replace(uint16_t slot, uint32_t expectedSlotGeneration, const NodeEntityRecord &record) {
		validateLayout();
		validateLiveSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		replace(slot, record);
	}

	void EntityPage::replace(uint16_t slot, const EdgeEntityRecord &record) {
		if (header_.record_kind != EntityPageRecordKind::EPRK_EDGE) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page does not store Edge records");
		}
		validateLayout();
		validateLiveSlot(slot);
		const EdgeEntityRecordCodec::Bytes encoded = EdgeEntityRecordCodec::encode(record);
		std::copy(encoded.begin(), encoded.end(), bytes_.begin() + recordOffset(slot));
		refreshChecksums();
	}

	void EntityPage::replace(uint16_t slot, uint32_t expectedSlotGeneration, const EdgeEntityRecord &record) {
		validateLayout();
		validateLiveSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		replace(slot, record);
	}

	void EntityPage::erase(uint16_t slot) {
		validateLayout();
		validateLiveSlot(slot);
		std::fill_n(bytes_.begin() + recordOffset(slot), header_.record_bytes, std::byte{});
		setLive(slot, false);
		--header_.live_count;
		refreshChecksums();
	}

	void EntityPage::erase(uint16_t slot, uint32_t expectedSlotGeneration) {
		validateLayout();
		validateLiveSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		erase(slot);
	}

	std::vector<std::byte> EntityPage::serialize() const {
		validateLayout();
		return bytes_;
	}

	void EntityPage::validateLayout() const {
		validateHeader(header_);
		const size_t recordAreaStart =
				bitmapOffset() + header_.bitmap_bytes + static_cast<size_t>(header_.slot_capacity) * sizeof(uint32_t);
		const size_t recordAreaEnd =
				recordAreaStart + static_cast<size_t>(header_.slot_capacity) * header_.record_bytes;
		uint16_t actualLive = 0U;
		for (uint16_t slot = 0U; slot < header_.slot_capacity; ++slot) {
			const size_t offset = recordOffset(slot);
			if (!isLive(slot)) {
				if (!std::all_of(bytes_.begin() + offset, bytes_.begin() + offset + header_.record_bytes,
								 [](std::byte value) { return value == std::byte{}; })) {
					throwEntityPageError(FormatError::FERR_INVALID_HEADER,
										 "relocatable Entity page free slot retains record bytes");
				}
				continue;
			}
			if (LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot)) == 0U) {
				throwEntityPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable Entity page live slot has no generation");
			}
			if (header_.record_kind == EntityPageRecordKind::EPRK_NODE) {
				static_cast<void>(
						NodeEntityRecordCodec::decode(std::span(bytes_).subspan(offset, header_.record_bytes)));
			} else {
				static_cast<void>(
						EdgeEntityRecordCodec::decode(std::span(bytes_).subspan(offset, header_.record_bytes)));
			}
			++actualLive;
		}
		const uint8_t unusedBits = static_cast<uint8_t>(header_.bitmap_bytes * 8U - header_.slot_capacity);
		const uint8_t tailMask = unusedBits == 0U ? 0U : static_cast<uint8_t>(0xFFU << (8U - unusedBits));
		const uint8_t invalid =
				static_cast<uint8_t>(actualLive != header_.live_count) |
				static_cast<uint8_t>(unusedBits != 0U &&
									 (std::to_integer<uint8_t>(bytes_[bitmapOffset() + header_.bitmap_bytes - 1U]) &
									  tailMask) != 0U) |
				static_cast<uint8_t>(!std::all_of(bytes_.begin() + recordAreaEnd, bytes_.end(),
												  [](std::byte value) { return value == std::byte{}; }));
		if (invalid != 0U) {
			throwEntityPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable Entity page bitmap, counters, or trailing bytes are invalid");
		}
	}

	void EntityPage::validateLiveSlot(uint16_t slot) const {
		if (slot >= header_.slot_capacity || !isLive(slot)) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE, "relocatable Entity page slot is unavailable");
		}
	}

	void EntityPage::validateSlotGeneration(uint16_t slot, uint32_t expectedSlotGeneration) const {
		if (expectedSlotGeneration == 0U ||
			LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot)) != expectedSlotGeneration) {
			throwEntityPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable Entity page slot generation is stale or unavailable");
		}
	}

	size_t EntityPage::bitmapOffset() const { return RSS_ENTITY_PAGE_HEADER_BYTES; }

	size_t EntityPage::slotGenerationOffset(uint16_t slot) const {
		return bitmapOffset() + header_.bitmap_bytes + static_cast<size_t>(slot) * sizeof(uint32_t);
	}

	size_t EntityPage::recordOffset(uint16_t slot) const {
		return bitmapOffset() + header_.bitmap_bytes + static_cast<size_t>(header_.slot_capacity) * sizeof(uint32_t) +
			   static_cast<size_t>(slot) * header_.record_bytes;
	}

	bool EntityPage::isLive(uint16_t slot) const {
		const size_t byteOffset = bitmapOffset() + slot / 8U;
		return (std::to_integer<uint8_t>(bytes_[byteOffset]) & static_cast<uint8_t>(1U << (slot % 8U))) != 0U;
	}

	void EntityPage::setLive(uint16_t slot, bool live) {
		const size_t byteOffset = bitmapOffset() + slot / 8U;
		const uint8_t mask = static_cast<uint8_t>(1U << (slot % 8U));
		const uint8_t current = std::to_integer<uint8_t>(bytes_[byteOffset]);
		bytes_[byteOffset] = static_cast<std::byte>(live ? current | mask : current & ~mask);
	}

	void EntityPage::setSlotGeneration(uint16_t slot, uint32_t generation) {
		LittleEndianCodec::writeU32(bytes_, slotGenerationOffset(slot), generation);
	}

	void EntityPage::refreshChecksums() {
		const EntityPageHeaderCodec::Bytes headerBytes = EntityPageHeaderCodec::encode(header_);
		std::copy(headerBytes.begin(), headerBytes.end(), bytes_.begin());
		LittleEndianCodec::writeU32(bytes_, PAGE_PAYLOAD_CRC_OFFSET,
									calculateCrc(std::span(bytes_).subspan(RSS_ENTITY_PAGE_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes_, PAGE_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes_, PAGE_HEADER_CRC_OFFSET,
									calculateCrc(std::span(bytes_).first(RSS_ENTITY_PAGE_HEADER_BYTES)));
	}

} // namespace graph::storage
