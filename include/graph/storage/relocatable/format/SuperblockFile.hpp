/**
 * @file SuperblockFile.hpp
 * @brief Durable A/B relocatable superblock storage over the cross-platform I/O layer.
 *
 * This is an isolated relocatable bootstrap component. It is not wired into the
 * retired fixed-record header and therefore cannot reinterpret a legacy file.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockPair.hpp"

namespace graph::storage {

	class SuperblockFile final {
	public:
		explicit SuperblockFile(std::shared_ptr<StorageIO> io);

		// Initializes a new relocatable file's fixed bootstrap area. It must not be
		// called for an existing or legacy database file.
		void initializeNew(const Superblock &initial);
		[[nodiscard]] Superblock recover() const;

		// Data pages referenced by candidate must have been written through this
		// StorageIO before publication. publish() enforces data-before-reference
		// durability with a sync before and after the inactive-slot write.
		void publish(const Superblock &candidate,
					 BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

		[[nodiscard]] SuperblockPair::SlotBytes readSlot(uint8_t slot) const;
		[[nodiscard]] const std::shared_ptr<StorageIO> &storageIO() const { return io_; }

		static constexpr uint64_t slotOffset(uint8_t slot) {
			return static_cast<uint64_t>(slot) * RSS_SUPERBLOCK_BYTES;
		}

	private:
		[[nodiscard]] SuperblockPair::Slots readSlotsLocked() const;
		[[nodiscard]] uint8_t inactiveSlotLocked(const SuperblockPair::Slots &slots) const;
		void writeSlotLocked(uint8_t slot, const SuperblockPair::SlotBytes &bytes, Generation generation) const;

		std::shared_ptr<StorageIO> io_;
		mutable std::mutex mutex_;
	};

} // namespace graph::storage
