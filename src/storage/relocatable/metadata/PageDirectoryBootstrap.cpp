/**
 * @file PageDirectoryBootstrap.cpp
 * @brief Codec for the physically addressed Page Directory bootstrap header.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"

#include <algorithm>
#include <limits>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t GENERATION_OFFSET = 8U;
		constexpr size_t COMMITTED_LSN_OFFSET = 16U;
		constexpr size_t DATA_ROOT_OFFSET = 24U;
		constexpr size_t METADATA_ROOT_OFFSET = 40U;
		constexpr size_t METADATA_DIRECTORY_ROOTS_OFFSET = 56U;
		constexpr size_t PAYLOAD_CRC_OFFSET =
				METADATA_DIRECTORY_ROOTS_OFFSET + RSS_METADATA_DIRECTORY_ROOT_COUNT * sizeof(uint64_t);
		constexpr size_t HEADER_CRC_OFFSET = PAYLOAD_CRC_OFFSET + sizeof(uint32_t);
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[noreturn]] void throwBootstrapError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void writePageRef(std::span<std::byte> bytes, size_t offset, const PhysicalPageRef &reference) {
			LittleEndianCodec::writeU64(bytes, offset, reference.physical_slot.value());
			if (!reference.physical_slot.isValid()) {
				LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t), 0U);
				return;
			}
			LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t),
									 (reference.generation.value() << 5U) | reference.frame_index);
		}

		[[nodiscard]] PhysicalPageRef readPageRef(std::span<const std::byte> bytes, size_t offset) {
			PhysicalPageRef reference;
			reference.physical_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, offset));
			const uint64_t packed = LittleEndianCodec::readU64(bytes, offset + sizeof(uint64_t));
			if (!reference.physical_slot.isValid()) {
				if (packed != 0U) {
					throwBootstrapError(FormatError::FERR_INVALID_HEADER,
										"an absent Page Directory bootstrap branch has location data");
				}
				return reference;
			}
			reference.frame_index = static_cast<uint8_t>(packed & 0x1FU);
			reference.generation = Generation(packed >> 5U);
			return reference;
		}

		void validatePageRef(const PhysicalPageRef &reference) {
			const bool hasSlot = reference.physical_slot.isValid();
			const bool hasLocation = reference.frame_index != 0U || reference.generation.isValid();
			if (!hasSlot && !hasLocation) {
				return;
			}
			if (!reference.isValid() || reference.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) {
				throwBootstrapError(FormatError::FERR_INVALID_HEADER,
									"invalid Page Directory bootstrap physical branch reference");
			}
		}

		void validateBootstrap(const PageDirectoryBootstrap &bootstrap) {
			if (!bootstrap.generation.isValid() || !bootstrap.committed_lsn.isValid()) {
				throwBootstrapError(FormatError::FERR_INVALID_HEADER,
									"Page Directory bootstrap needs a generation and committed LSN");
			}
			validatePageRef(bootstrap.data_page_directory_root);
			validatePageRef(bootstrap.metadata_page_directory_root);
		}

		void writeHeader(std::span<std::byte> bytes, const PageDirectoryBootstrap &bootstrap, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_PAGE_DIRECTORY_BOOTSTRAP_MAGIC);
			LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_PAGE_DIRECTORY_BOOTSTRAP_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, bootstrap.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, bootstrap.committed_lsn.value());
			writePageRef(bytes, DATA_ROOT_OFFSET, bootstrap.data_page_directory_root);
			writePageRef(bytes, METADATA_ROOT_OFFSET, bootstrap.metadata_page_directory_root);
			for (size_t index = 0U; index < bootstrap.metadata_directory_roots.size(); ++index) {
				LittleEndianCodec::writeU64(bytes, METADATA_DIRECTORY_ROOTS_OFFSET + index * sizeof(uint64_t),
									 bootstrap.metadata_directory_roots[index].value());
			}
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
		}

	} // namespace

	PageDirectoryBootstrapCodec::Bytes PageDirectoryBootstrapCodec::encode(const PageDirectoryBootstrap &bootstrap) {
		validateBootstrap(bootstrap);
		Bytes bytes{};
		writeHeader(bytes, bootstrap, 0U, 0U);
		const uint32_t payloadCrc =
				calculateCrc(std::span<const std::byte>(bytes).subspan(RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES));
		writeHeader(bytes, bootstrap, payloadCrc, 0U);
		const uint32_t headerCrc =
				calculateCrc(std::span<const std::byte>(bytes).first(RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES));
		writeHeader(bytes, bootstrap, payloadCrc, headerCrc);
		return bytes;
	}

	PageDirectoryBootstrap PageDirectoryBootstrapCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_SIZE) {
			throwBootstrapError(FormatError::FERR_BUFFER_TOO_SMALL, "Page Directory bootstrap is truncated");
		}
		if (bytes.size() != RSS_PAGE_SIZE) {
			throwBootstrapError(FormatError::FERR_INVALID_HEADER, "Page Directory bootstrap has an unexpected size");
		}
		std::array<std::byte, RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwBootstrapError(FormatError::FERR_CHECKSUM_MISMATCH,
									"Page Directory bootstrap header checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_PAGE_DIRECTORY_BOOTSTRAP_MAGIC) {
			throwBootstrapError(FormatError::FERR_INVALID_MAGIC, "Page Directory bootstrap magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_PAGE_DIRECTORY_BOOTSTRAP_VERSION) {
			throwBootstrapError(FormatError::FERR_UNSUPPORTED_VERSION,
									"unsupported Page Directory bootstrap version");
		}
		if (LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES) {
			throwBootstrapError(FormatError::FERR_INVALID_HEADER, "invalid Page Directory bootstrap dimensions");
		}
		if (calculateCrc(bytes.subspan(RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwBootstrapError(FormatError::FERR_CHECKSUM_MISMATCH,
									"Page Directory bootstrap payload checksum does not match");
		}
		if (!std::all_of(bytes.begin() + RSS_PAGE_DIRECTORY_BOOTSTRAP_HEADER_BYTES, bytes.end(),
						 [](std::byte value) { return value == std::byte{}; })) {
			throwBootstrapError(FormatError::FERR_RESERVED_BITS,
									"Page Directory bootstrap has nonzero reserved payload bytes");
		}

		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		bootstrap.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		bootstrap.data_page_directory_root = readPageRef(bytes, DATA_ROOT_OFFSET);
		bootstrap.metadata_page_directory_root = readPageRef(bytes, METADATA_ROOT_OFFSET);
		for (size_t index = 0U; index < bootstrap.metadata_directory_roots.size(); ++index) {
			bootstrap.metadata_directory_roots[index] =
					MetadataPageId(LittleEndianCodec::readU64(bytes, METADATA_DIRECTORY_ROOTS_OFFSET + index * sizeof(uint64_t)));
		}
		validateBootstrap(bootstrap);
		return bootstrap;
	}

} // namespace graph::storage
