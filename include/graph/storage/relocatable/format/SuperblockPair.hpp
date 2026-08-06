/**
 * @file SuperblockPair.hpp
 * @brief In-memory model of copy-on-write A/B superblock publication.
 *
 * This class owns no database file or WAL. It establishes the required
 * ordering contract for a later I/O integration: write an already durable
 * metadata generation to the inactive superblock, then let recovery select
 * the highest valid complete generation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>

#include "graph/storage/relocatable/format/Superblock.hpp"

namespace graph::storage {

	// All enumerators have a type prefix to stay safe in Windows builds.
	enum class BootstrapPublishFailpoint : uint8_t {
		BPF_NONE = 0,
		BPF_BEFORE_INACTIVE_WRITE = 1,
		BPF_AFTER_PARTIAL_INACTIVE_WRITE = 2,
		BPF_AFTER_INACTIVE_WRITE = 3,
	};

	class BootstrapPublishInterrupted final : public std::runtime_error {
	public:
		explicit BootstrapPublishInterrupted(const char *message) : std::runtime_error(message) {}
	};

	class SuperblockPair final {
	public:
		using SlotBytes = std::array<std::byte, RSS_SUPERBLOCK_BYTES>;
		using Slots = std::array<SlotBytes, 2>;

		explicit SuperblockPair(const Superblock &initial);
		explicit SuperblockPair(const Slots &slots);

		[[nodiscard]] Superblock recover() const;
		[[nodiscard]] SlotBytes slotBytes(uint8_t slot) const;

		// The caller must have made every root referenced by candidate durable
		// before this switch. Candidate generation and checkpoint LSN must advance.
		void publish(const Superblock &candidate,
					 BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

	private:
		[[nodiscard]] Superblock recoverLocked() const;
		[[nodiscard]] uint8_t inactiveSlotLocked() const;

		mutable std::mutex mutex_;
		Slots slots_{};
	};

} // namespace graph::storage
