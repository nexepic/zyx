/**
 * @file SuperblockPair.cpp
 * @brief Copy-on-write A/B superblock publication model for relocatable metadata.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/format/SuperblockPair.hpp"

#include <algorithm>
#include <optional>

namespace graph::storage {

	namespace {
		// A simulated torn write must
		// not overwrite it, otherwise a partly written candidate can validate when
		// the unwritten tail happens to equal the prior Superblock.
		constexpr size_t TORN_HEADER_WRITE_BYTES = RSS_SUPERBLOCK_HEADER_CRC_OFFSET;
		static_assert(TORN_HEADER_WRITE_BYTES < RSS_SUPERBLOCK_HEADER_BYTES);

		[[noreturn]] void throwPublishError(const char *message) {
			throw FormatException(FormatError::FERR_INVALID_STATE, message);
		}

		[[nodiscard]] std::optional<Superblock> tryDecode(const SuperblockPair::SlotBytes &bytes) {
			try {
				return SuperblockCodec::decode(bytes);
			} catch (const FormatException &) {
				return std::nullopt;
			}
		}

		[[nodiscard]] bool isKnownFailpoint(BootstrapPublishFailpoint failpoint) {
			return failpoint == BootstrapPublishFailpoint::BPF_NONE ||
				   failpoint == BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE ||
				   failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE ||
				   failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		}

	} // namespace

	SuperblockPair::SuperblockPair(const Superblock &initial) {
		const auto encoded = SuperblockCodec::encode(initial);
		slots_[0] = encoded;
		slots_[1] = encoded;
	}

	SuperblockPair::SuperblockPair(const Slots &slots) : slots_(slots) {}

	Superblock SuperblockPair::recover() const {
		std::lock_guard lock(mutex_);
		return recoverLocked();
	}

	SuperblockPair::SlotBytes SuperblockPair::slotBytes(uint8_t slot) const {
		if (slot >= slots_.size()) {
			throwPublishError("relocatable superblock slot index is invalid");
		}
		std::lock_guard lock(mutex_);
		return slots_[slot];
	}

	void SuperblockPair::publish(const Superblock &candidate, BootstrapPublishFailpoint failpoint) {
		if (!isKnownFailpoint(failpoint)) {
			throwPublishError("relocatable superblock publication failpoint is invalid");
		}
		const auto encoded = SuperblockCodec::encode(candidate);
		std::lock_guard lock(mutex_);
		const Superblock current = recoverLocked();
		if (candidate.generation <= current.generation || candidate.checkpoint_lsn <= current.checkpoint_lsn) {
			throwPublishError("relocatable superblock publication must advance generation and checkpoint LSN");
		}
		if (failpoint == BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE) {
			throw BootstrapPublishInterrupted("injected crash before inactive relocatable superblock write");
		}

		const uint8_t targetSlot = inactiveSlotLocked();
		if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE) {
			// A torn header cannot validate because its checksum remains from the prior generation.
			std::copy_n(encoded.begin(), TORN_HEADER_WRITE_BYTES, slots_[targetSlot].begin());
			throw BootstrapPublishInterrupted("injected crash during inactive relocatable superblock write");
		}

		slots_[targetSlot] = encoded;
		if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE) {
			throw BootstrapPublishInterrupted("injected crash after inactive relocatable superblock write");
		}
	}

	Superblock SuperblockPair::recoverLocked() const { return SuperblockCodec::selectNewest(slots_[0], slots_[1]); }

	uint8_t SuperblockPair::inactiveSlotLocked() const {
		const std::optional<Superblock> first = tryDecode(slots_[0]);
		const std::optional<Superblock> second = tryDecode(slots_[1]);
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

} // namespace graph::storage
