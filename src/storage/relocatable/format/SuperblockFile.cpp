/**
 * @file SuperblockFile.cpp
 * @brief Durable cross-platform file implementation of relocatable A/B superblocks.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace graph::storage {

	namespace {
		// Keep a simulated torn write before the header checksum.
		// Writing through that checksum can make an incomplete candidate validate
		// when the remaining bytes match the prior Superblock.
		constexpr size_t TORN_HEADER_WRITE_BYTES = RSS_SUPERBLOCK_HEADER_CRC_OFFSET;
		static_assert(TORN_HEADER_WRITE_BYTES < RSS_SUPERBLOCK_HEADER_BYTES);

		[[noreturn]] void throwSuperblockFileError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isKnownFailpoint(BootstrapPublishFailpoint failpoint) {
			return failpoint == BootstrapPublishFailpoint::BPF_NONE ||
				   failpoint == BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE ||
				   failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE ||
				   failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		}

		[[nodiscard]] std::optional<Superblock> tryDecode(const SuperblockPair::SlotBytes &bytes) {
			try {
				return SuperblockCodec::decode(bytes);
			} catch (const FormatException &) {
				return std::nullopt;
			}
		}

	} // namespace

	SuperblockFile::SuperblockFile(std::shared_ptr<StorageIO> io) : io_(std::move(io)) {
		if (!io_) {
			throw std::invalid_argument("relocatable SuperblockFile requires StorageIO");
		}
	}

	void SuperblockFile::initializeNew(const Superblock &initial) {
		const SuperblockPair::SlotBytes encoded = SuperblockCodec::encode(initial);
		std::lock_guard lock(mutex_);
		writeSlotLocked(0, encoded, initial.generation);
		writeSlotLocked(1, encoded, initial.generation);

		// Reserve the immutable bootstrap region before any physical slot is allocated.
		const std::byte zero{};
		io_->writeAt(RSS_FILE_BOOTSTRAP_BYTES - 1U, &zero, sizeof(zero));
		io_->sync();
	}

	Superblock SuperblockFile::recover() const {
		std::lock_guard lock(mutex_);
		const SuperblockPair::Slots slots = readSlotsLocked();
		return SuperblockCodec::selectNewest(slots[0], slots[1]);
	}

	void SuperblockFile::publish(const Superblock &candidate, BootstrapPublishFailpoint failpoint) {
		if (!isKnownFailpoint(failpoint)) {
			throwSuperblockFileError(FormatError::FERR_INVALID_STATE, "relocatable superblock file failpoint is invalid");
		}
		const SuperblockPair::SlotBytes encoded = SuperblockCodec::encode(candidate);
		std::lock_guard lock(mutex_);
		const SuperblockPair::Slots slots = readSlotsLocked();
		const Superblock current = SuperblockCodec::selectNewest(slots[0], slots[1]);
		if (candidate.generation <= current.generation || candidate.checkpoint_lsn <= current.checkpoint_lsn) {
			throwSuperblockFileError(FormatError::FERR_INVALID_STATE,
									 "relocatable superblock file publication must advance generation and checkpoint LSN");
		}
		if (failpoint == BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE) {
			throw BootstrapPublishInterrupted("injected crash before durable inactive relocatable superblock write");
		}

		// Ensure every candidate root written through this I/O instance is durable before it becomes reachable.
		io_->sync(StorageIOSyncKind::SIOSK_DATA_FENCE);
		const uint8_t targetSlot = inactiveSlotLocked(slots);
		if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE) {
			io_->writeAt(slotOffset(targetSlot), encoded.data(), TORN_HEADER_WRITE_BYTES);
			io_->sync(StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE);
			throw BootstrapPublishInterrupted("injected crash during durable inactive relocatable superblock write");
		}

		writeSlotLocked(targetSlot, encoded, candidate.generation);
		io_->sync(StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE);
		if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE) {
			throw BootstrapPublishInterrupted("injected crash after durable inactive relocatable superblock write");
		}
	}

	SuperblockPair::SlotBytes SuperblockFile::readSlot(uint8_t slot) const {
		if (slot >= 2U) {
			throwSuperblockFileError(FormatError::FERR_INVALID_HEADER, "relocatable superblock file slot index is invalid");
		}
		std::lock_guard lock(mutex_);
		SuperblockPair::SlotBytes bytes{};
		const size_t bytesRead = io_->readAt(slotOffset(slot), bytes.data(), bytes.size());
		if (bytesRead != bytes.size()) {
			throwSuperblockFileError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable superblock file slot is truncated");
		}
		return bytes;
	}

	SuperblockPair::Slots SuperblockFile::readSlotsLocked() const {
		SuperblockPair::Slots slots{};
		for (uint8_t slot = 0; slot < slots.size(); ++slot) {
			// A Superblock is a self-authenticating control record. Read it outside
			// this handle's page cache so a separately opened writer is observable
			// before a stale transaction decides whether it can rebase.
			const size_t bytesRead = io_->readAtSequential(slotOffset(slot), slots[slot].data(), slots[slot].size());
			if (bytesRead != slots[slot].size()) {
				throwSuperblockFileError(FormatError::FERR_BUFFER_TOO_SMALL,
										 "relocatable superblock file bootstrap is truncated");
			}
		}
		return slots;
	}

	uint8_t SuperblockFile::inactiveSlotLocked(const SuperblockPair::Slots &slots) const {
		const std::optional<Superblock> first = tryDecode(slots[0]);
		const std::optional<Superblock> second = tryDecode(slots[1]);
		if (!first.has_value()) {
			return 0;
		}
		if (!second.has_value()) {
			return 1;
		}
		if (first->generation == second->generation) {
			return 1;
		}
		return first->generation < second->generation ? 0 : 1;
	}

	void SuperblockFile::writeSlotLocked(uint8_t slot, const SuperblockPair::SlotBytes &bytes,
										   Generation generation) const {
		// A control-record write invalidates cached views before the direct I/O.
		// This prevents a failed short Superblock write from being replayed later
		// as a complete dirty Buffer Pool page.
		io_->writeControlRecord({slotOffset(slot), generation.value()}, 0U, bytes);
	}

} // namespace graph::storage
