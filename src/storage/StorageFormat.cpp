/**
 * @file StorageFormat.cpp
 * @brief Bounded fail-closed detection for the native relocatable format.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/StorageFormat.hpp"

#include <array>
#include <cstddef>
#include <optional>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] bool hasRelocatableMagic(const std::array<std::byte, sizeof(uint32_t)> &bytes) {
			const uint32_t magic = static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[0])) |
								   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[1])) << 8U) |
								   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[2])) << 16U) |
								   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[3])) << 24U);
			return magic == RSS_SUPERBLOCK_MAGIC;
		}

		[[nodiscard]] std::optional<Superblock> readRelocatableSlot(const StorageIO &io, uint64_t offset, uint64_t fileSize) {
			if (offset > fileSize || fileSize - offset < RSS_SUPERBLOCK_BYTES) {
				return std::nullopt;
			}
			// A short positioned read leaves zero-initialized bytes, which cannot
			// validate a checksummed superblock and therefore remains fail-closed.
			std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes{};
			static_cast<void>(io.readAt(offset, bytes.data(), bytes.size()));
			try {
				return SuperblockCodec::decode(bytes);
			} catch (const FormatException &) {
				return std::nullopt;
			}
		}

		[[nodiscard]] bool slotStartsWithRelocatableMagic(const StorageIO &io, uint64_t offset, uint64_t fileSize) {
			if (offset > fileSize || fileSize - offset < sizeof(uint32_t)) {
				return false;
			}
			std::array<std::byte, sizeof(uint32_t)> bytes{};
			static_cast<void>(io.readAt(offset, bytes.data(), bytes.size()));
			return hasRelocatableMagic(bytes);
		}

	} // namespace

	StorageFormatInfo StorageFormatProbe::inspect(const StorageIO &io) {
		const uint64_t fileSize = io.fileSize();
		if (fileSize == 0U) {
			return {StorageFormatKind::SFM_EMPTY, fileSize};
		}
		if (fileSize < sizeof(uint32_t)) {
			return {StorageFormatKind::SFM_TRUNCATED, fileSize};
		}

		// Disk-controlled bytes can request at most two fixed 4 KiB reads. No
		// allocation or offset calculation is derived from the file content.
		const bool firstHasRelocatableMagic = slotStartsWithRelocatableMagic(io, 0, fileSize);
		const bool secondHasRelocatableMagic = slotStartsWithRelocatableMagic(io, RSS_SUPERBLOCK_BYTES, fileSize);
		if (!firstHasRelocatableMagic && !secondHasRelocatableMagic) {
			return {StorageFormatKind::SFM_UNRECOGNIZED, fileSize};
		}

		const bool completeBootstrap = fileSize >= RSS_FILE_BOOTSTRAP_BYTES;
		const bool hasValidSlot = readRelocatableSlot(io, 0, fileSize).has_value() ||
								  readRelocatableSlot(io, RSS_SUPERBLOCK_BYTES, fileSize).has_value();
		if (completeBootstrap && hasValidSlot) {
			return {StorageFormatKind::SFM_RELOCATABLE, fileSize};
		}
		return {StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE, fileSize};
	}

	const char *StorageFormatProbe::describe(StorageFormatKind kind) {
		switch (kind) {
			case StorageFormatKind::SFM_EMPTY:
				return "empty";
			case StorageFormatKind::SFM_RELOCATABLE:
				return "relocatable";
			case StorageFormatKind::SFM_RELOCATABLE_INCOMPLETE:
				return "incomplete relocatable";
			case StorageFormatKind::SFM_TRUNCATED:
				return "truncated";
			case StorageFormatKind::SFM_UNRECOGNIZED:
				return "unrecognized";
		}
		return "invalid";
	}

} // namespace graph::storage
