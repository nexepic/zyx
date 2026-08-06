/**
 * @file VectorPage.cpp
 * @brief Fixed-record raw/PQ vector page codec for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorPage.hpp"

#include <algorithm>
#include <limits>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t KIND_OFFSET = 8;
		constexpr size_t FLAGS_OFFSET = 10;
		constexpr size_t RECORD_BYTES_OFFSET = 12;
		constexpr size_t SLOT_CAPACITY_OFFSET = 16;
		constexpr size_t LIVE_COUNT_OFFSET = 18;
		constexpr size_t BITMAP_BYTES_OFFSET = 20;
		constexpr size_t RESERVED_OFFSET = 22;
		constexpr size_t INDEX_ID_OFFSET = 24;
		constexpr size_t PAGE_ID_OFFSET = 32;
		constexpr size_t GENERATION_OFFSET = 40;
		constexpr size_t PAYLOAD_CRC_OFFSET = 48;
		constexpr size_t HEADER_CRC_OFFSET = 52;
		constexpr size_t COMMITTED_LSN_OFFSET = 56;
		constexpr size_t CODEBOOK_GENERATION_OFFSET = 64;

		[[noreturn]] void throwVectorPageError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isVectorStorageKind(VectorStorageKind kind) {
			return kind == VectorStorageKind::VSK_RAW || kind == VectorStorageKind::VSK_PQ;
		}

		[[nodiscard]] uint16_t requiredBitmapBytes(uint16_t slotCapacity) {
			return static_cast<uint16_t>((static_cast<uint32_t>(slotCapacity) + 7U) / 8U);
		}

		[[nodiscard]] uint64_t slotGenerationBytes(uint16_t slotCapacity) {
			return static_cast<uint64_t>(slotCapacity) * sizeof(uint32_t);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateHeader(const VectorPageHeader &header) {
			const bool raw = header.kind == VectorStorageKind::VSK_RAW;
			const uint64_t recordArea = static_cast<uint64_t>(header.slot_capacity) * header.record_bytes;
			const uint64_t payloadBytes = static_cast<uint64_t>(RSS_VECTOR_PAGE_HEADER_BYTES) + header.bitmap_bytes +
										  slotGenerationBytes(header.slot_capacity) + recordArea;
			const uint8_t invalid =
					static_cast<uint8_t>(header.index_id == 0U) | static_cast<uint8_t>(!header.page_id.isValid()) |
					static_cast<uint8_t>(!header.generation.isValid()) |
					static_cast<uint8_t>(!header.committed_lsn.isValid()) |
					static_cast<uint8_t>(!isVectorStorageKind(header.kind)) |
					static_cast<uint8_t>(raw == header.codebook_generation.isValid()) |
					static_cast<uint8_t>(header.record_bytes == 0U) | static_cast<uint8_t>(header.slot_capacity == 0U) |
					static_cast<uint8_t>(header.live_count > header.slot_capacity) |
					static_cast<uint8_t>(header.bitmap_bytes != requiredBitmapBytes(header.slot_capacity)) |
					static_cast<uint8_t>(payloadBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
			if (invalid != 0U) {
				throwVectorPageError(FormatError::FERR_INVALID_HEADER,
									 "relocatable vector page header has invalid identity or fixed-record layout");
			}
		}

		void validateOwner(const PageOwnerHeader &owner, const VectorPageHeader &header, bool allowRetired) {
			const PageOwnerKind expectedKind = header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
																						 : PageOwnerKind::POK_VECTOR_PQ;
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != header.page_id) |
									static_cast<uint8_t>(owner.owner_id != header.index_id) |
									static_cast<uint8_t>(owner.kind != expectedKind) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
									static_cast<uint8_t>(!allowRetired && owner.flags != 0U) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(owner.generation != header.generation) |
									static_cast<uint8_t>(owner.committed_lsn != header.committed_lsn);
			if (invalid != 0U) {
				throwVectorPageError(FormatError::FERR_INVALID_STATE,
									 "relocatable vector page owner disagrees with its fixed-record header");
			}
		}

	} // namespace

	VectorPageHeaderCodec::Bytes VectorPageHeaderCodec::encode(const VectorPageHeader &header) {
		validateHeader(header);
		Bytes bytes{};
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_VECTOR_PAGE_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_VECTOR_PAGE_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_VECTOR_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(header.kind));
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, RECORD_BYTES_OFFSET, header.record_bytes);
		LittleEndianCodec::writeU16(bytes, SLOT_CAPACITY_OFFSET, header.slot_capacity);
		LittleEndianCodec::writeU16(bytes, LIVE_COUNT_OFFSET, header.live_count);
		LittleEndianCodec::writeU16(bytes, BITMAP_BYTES_OFFSET, header.bitmap_bytes);
		LittleEndianCodec::writeU16(bytes, RESERVED_OFFSET, 0);
		LittleEndianCodec::writeU64(bytes, INDEX_ID_OFFSET, header.index_id);
		LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, header.page_id.value());
		LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, header.generation.value());
		LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, header.committed_lsn.value());
		LittleEndianCodec::writeU64(bytes, CODEBOOK_GENERATION_OFFSET, header.codebook_generation.value());
		return bytes;
	}

	VectorPageHeader VectorPageHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_VECTOR_PAGE_HEADER_BYTES) {
			throwVectorPageError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable vector page header is truncated");
		}
		if (bytes.size() != RSS_VECTOR_PAGE_HEADER_BYTES) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER, "relocatable vector page header has an invalid length");
		}
		const uint8_t invalidFixed =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_VECTOR_PAGE_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_VECTOR_PAGE_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_VECTOR_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, RESERVED_OFFSET) != 0U);
		if (invalidFixed != 0U) {
			throwVectorPageError(FormatError::FERR_RESERVED_BITS,
								 "relocatable vector page header has invalid fixed or reserved fields");
		}
		VectorPageHeader header;
		header.kind = static_cast<VectorStorageKind>(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		header.record_bytes = LittleEndianCodec::readU32(bytes, RECORD_BYTES_OFFSET);
		header.slot_capacity = LittleEndianCodec::readU16(bytes, SLOT_CAPACITY_OFFSET);
		header.live_count = LittleEndianCodec::readU16(bytes, LIVE_COUNT_OFFSET);
		header.bitmap_bytes = LittleEndianCodec::readU16(bytes, BITMAP_BYTES_OFFSET);
		header.index_id = LittleEndianCodec::readU64(bytes, INDEX_ID_OFFSET);
		header.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		header.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		header.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		header.codebook_generation = Generation(LittleEndianCodec::readU64(bytes, CODEBOOK_GENERATION_OFFSET));
		validateHeader(header);
		return header;
	}

	VectorPage::VectorPage(std::vector<std::byte> bytes, VectorPageHeader header) :
		bytes_(std::move(bytes)), header_(std::move(header)) {}

	VectorPage::VectorPage(VectorPage &&other) noexcept :
		bytes_(std::move(other.bytes_)), header_(std::move(other.header_)) {}

	VectorPage &VectorPage::operator=(VectorPage &&other) noexcept {
		if (this != &other) {
			bytes_ = std::move(other.bytes_);
			header_ = std::move(other.header_);
		}
		return *this;
	}

	VectorPage VectorPage::create(const PageOwnerHeader &owner, const VectorPageHeader &header) {
		validateHeader(header);
		validateOwner(owner, header, false);
		VectorPage page(std::vector<std::byte>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES), header);
		const VectorPageHeaderCodec::Bytes encodedHeader = VectorPageHeaderCodec::encode(header);
		std::copy(encodedHeader.begin(), encodedHeader.end(), page.bytes_.begin());
		page.refreshChecksums();
		return page;
	}

	VectorPage VectorPage::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			throwVectorPageError(bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																				   : FormatError::FERR_INVALID_HEADER,
								 "relocatable vector page payload has an invalid length");
		}
		std::vector<std::byte> copied(bytes.begin(), bytes.end());
		std::array<std::byte, RSS_VECTOR_PAGE_HEADER_BYTES> encodedHeader{};
		std::copy_n(copied.begin(), encodedHeader.size(), encodedHeader.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(encodedHeader, HEADER_CRC_OFFSET);
		const uint32_t expectedPayloadCrc = LittleEndianCodec::readU32(encodedHeader, PAYLOAD_CRC_OFFSET);
		LittleEndianCodec::writeU32(encodedHeader, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(encodedHeader) != expectedHeaderCrc ||
			calculateCrc(std::span(copied).subspan(RSS_VECTOR_PAGE_HEADER_BYTES)) != expectedPayloadCrc) {
			throwVectorPageError(FormatError::FERR_CHECKSUM_MISMATCH,
								 "relocatable vector page checksum does not match its fixed-record payload");
		}
		VectorPage page(std::move(copied), VectorPageHeaderCodec::decode(encodedHeader));
		// A pinned snapshot can legitimately resolve a retired physical frame.
		validateOwner(owner, page.header_, true);
		page.validateLayout();
		return page;
	}

	VectorPageHeader VectorPage::header() const { return header_; }

	VectorPage VectorPage::rebindOwner(const PageOwnerHeader &owner) const {
		validateLayout();
		VectorPageHeader rebound = header_;
		rebound.generation = owner.generation;
		rebound.committed_lsn = owner.committed_lsn;
		validateOwner(owner, rebound, false);
		VectorPage result(bytes_, rebound);
		result.refreshChecksums();
		result.validateLayout();
		return result;
	}

	uint16_t VectorPage::insert(std::span<const std::byte> record) {
		validateLayout();
		if (record.size() != header_.record_bytes) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable vector record does not match its page stride");
		}
		for (uint16_t slot = 0; slot < header_.slot_capacity; ++slot) {
			if (!isLive(slot)) {
				const uint32_t previousGeneration = LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
				if (previousGeneration == std::numeric_limits<uint32_t>::max()) {
					throwVectorPageError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										 "relocatable vector page slot generation is exhausted");
				}
				std::copy(record.begin(), record.end(), bytes_.begin() + recordOffset(slot));
				setSlotGeneration(slot, previousGeneration + 1U);
				setLive(slot, true);
				++header_.live_count;
				refreshChecksums();
				return slot;
			}
		}
		throwVectorPageError(FormatError::FERR_INVALID_STATE, "relocatable vector page has no free fixed-record slot");
	}

	std::vector<std::byte> VectorPage::read(uint16_t slot) const {
		validateLayout();
		validateSlot(slot);
		const size_t offset = recordOffset(slot);
		return {bytes_.begin() + offset, bytes_.begin() + offset + header_.record_bytes};
	}

	std::vector<std::byte> VectorPage::read(uint16_t slot, uint32_t expectedSlotGeneration) const {
		validateLayout();
		validateSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		const size_t offset = recordOffset(slot);
		return {bytes_.begin() + offset, bytes_.begin() + offset + header_.record_bytes};
	}

	uint32_t VectorPage::slotGeneration(uint16_t slot) const {
		validateLayout();
		validateSlot(slot);
		return LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot));
	}

	std::optional<uint16_t> VectorPage::findVectorId(uint64_t vectorId) const {
		validateLayout();
		if (vectorId == 0U || header_.record_bytes < sizeof(uint64_t)) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable Vector page cannot scan an invalid ID or a record without an ID prefix");
		}
		for (uint16_t slot = 0; slot < header_.slot_capacity; ++slot) {
			if (isLive(slot) && LittleEndianCodec::readU64(bytes_, recordOffset(slot)) == vectorId) {
				return slot;
			}
		}
		return std::nullopt;
	}

	void VectorPage::overwrite(uint16_t slot, std::span<const std::byte> record) {
		validateLayout();
		validateSlot(slot);
		if (record.size() != header_.record_bytes) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable vector record does not match its page stride");
		}
		std::copy(record.begin(), record.end(), bytes_.begin() + recordOffset(slot));
		refreshChecksums();
	}

	void VectorPage::overwrite(uint16_t slot, uint32_t expectedSlotGeneration, std::span<const std::byte> record) {
		validateLayout();
		validateSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		if (record.size() != header_.record_bytes) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable vector record does not match its page stride");
		}
		std::copy(record.begin(), record.end(), bytes_.begin() + recordOffset(slot));
		refreshChecksums();
	}

	void VectorPage::erase(uint16_t slot) {
		validateLayout();
		validateSlot(slot);
		const size_t offset = recordOffset(slot);
		std::fill_n(bytes_.begin() + offset, header_.record_bytes, std::byte{});
		setLive(slot, false);
		--header_.live_count;
		refreshChecksums();
	}

	void VectorPage::erase(uint16_t slot, uint32_t expectedSlotGeneration) {
		validateLayout();
		validateSlot(slot);
		validateSlotGeneration(slot, expectedSlotGeneration);
		const size_t offset = recordOffset(slot);
		std::fill_n(bytes_.begin() + offset, header_.record_bytes, std::byte{});
		setLive(slot, false);
		--header_.live_count;
		refreshChecksums();
	}

	std::vector<std::byte> VectorPage::serialize() const {
		validateLayout();
		return bytes_;
	}

	void VectorPage::validateLayout() const {
		validateHeader(header_);
		const size_t recordAreaStart =
				bitmapOffset() + header_.bitmap_bytes + slotGenerationBytes(header_.slot_capacity);
		const size_t recordAreaEnd =
				recordAreaStart + static_cast<size_t>(header_.slot_capacity) * header_.record_bytes;
		uint16_t actualLiveCount = 0;
		for (uint16_t slot = 0; slot < header_.slot_capacity; ++slot) {
			if (isLive(slot)) {
				++actualLiveCount;
				if (LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot)) == 0U) {
					throwVectorPageError(FormatError::FERR_INVALID_HEADER,
										 "relocatable vector page live slot has no generation");
				}
			}
		}
		const uint8_t unusedBitmapBits = static_cast<uint8_t>(header_.bitmap_bytes * 8U - header_.slot_capacity);
		const uint8_t bitmapTailMask =
				unusedBitmapBits == 0U ? 0U : static_cast<uint8_t>(0xFFU << (8U - unusedBitmapBits));
		const uint8_t invalid =
				static_cast<uint8_t>(actualLiveCount != header_.live_count) |
				static_cast<uint8_t>(unusedBitmapBits != 0U &&
									 (std::to_integer<uint8_t>(bytes_[bitmapOffset() + header_.bitmap_bytes - 1U]) &
									  bitmapTailMask) != 0U) |
				static_cast<uint8_t>(!std::all_of(bytes_.begin() + recordAreaEnd, bytes_.end(),
												  [](std::byte value) { return value == std::byte{}; }));
		if (invalid != 0U) {
			throwVectorPageError(FormatError::FERR_INVALID_HEADER,
								 "relocatable vector page bitmap, live count, or trailing padding is invalid");
		}
	}

	void VectorPage::validateSlot(uint16_t slot) const {
		if (slot >= header_.slot_capacity || !isLive(slot)) {
			throwVectorPageError(FormatError::FERR_INVALID_STATE, "relocatable vector page slot is unavailable");
		}
	}

	void VectorPage::validateSlotGeneration(uint16_t slot, uint32_t expectedSlotGeneration) const {
		if (expectedSlotGeneration == 0U ||
			LittleEndianCodec::readU32(bytes_, slotGenerationOffset(slot)) != expectedSlotGeneration) {
			throwVectorPageError(FormatError::FERR_INVALID_STATE,
								 "relocatable vector page slot generation is stale or unavailable");
		}
	}

	size_t VectorPage::bitmapOffset() const { return RSS_VECTOR_PAGE_HEADER_BYTES; }

	size_t VectorPage::slotGenerationOffset(uint16_t slot) const {
		return bitmapOffset() + header_.bitmap_bytes + static_cast<size_t>(slot) * sizeof(uint32_t);
	}

	size_t VectorPage::recordOffset(uint16_t slot) const {
		return bitmapOffset() + header_.bitmap_bytes + slotGenerationBytes(header_.slot_capacity) +
			   static_cast<size_t>(slot) * header_.record_bytes;
	}

	bool VectorPage::isLive(uint16_t slot) const {
		const size_t byteOffset = bitmapOffset() + slot / 8U;
		return (std::to_integer<uint8_t>(bytes_[byteOffset]) & static_cast<uint8_t>(1U << (slot % 8U))) != 0U;
	}

	void VectorPage::setLive(uint16_t slot, bool live) {
		const size_t byteOffset = bitmapOffset() + slot / 8U;
		const uint8_t mask = static_cast<uint8_t>(1U << (slot % 8U));
		const uint8_t current = std::to_integer<uint8_t>(bytes_[byteOffset]);
		bytes_[byteOffset] = static_cast<std::byte>(live ? current | mask : current & ~mask);
	}

	void VectorPage::setSlotGeneration(uint16_t slot, uint32_t generation) {
		LittleEndianCodec::writeU32(bytes_, slotGenerationOffset(slot), generation);
	}

	void VectorPage::refreshChecksums() {
		VectorPageHeader encodedHeader = header_;
		const VectorPageHeaderCodec::Bytes headerBytes = VectorPageHeaderCodec::encode(encodedHeader);
		std::copy(headerBytes.begin(), headerBytes.end(), bytes_.begin());
		LittleEndianCodec::writeU32(bytes_, PAYLOAD_CRC_OFFSET,
									calculateCrc(std::span(bytes_).subspan(RSS_VECTOR_PAGE_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes_, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes_, HEADER_CRC_OFFSET,
									calculateCrc(std::span(bytes_).first(RSS_VECTOR_PAGE_HEADER_BYTES)));
	}

} // namespace graph::storage
