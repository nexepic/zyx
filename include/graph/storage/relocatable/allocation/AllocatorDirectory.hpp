/**
 * @file AllocatorDirectory.hpp
 * @brief Copy-on-write free-space directory for fixed physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <optional>

#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

namespace graph::storage {

	class MetadataWriteBatch;

	struct AllocatorReservation {
		Superblock superblock;
		PhysicalSlotId physical_slot{};
	};

	class AllocatorDirectory final {
	public:
		AllocatorDirectory(MetadataPageFile &pages, MetadataPageAllocator &allocator);

		[[nodiscard]] bool isAllocated(const Superblock &superblock, PhysicalSlotId slot) const;
		[[nodiscard]] std::optional<PhysicalSlotId>
		findFirstFree(const Superblock &superblock, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot) const;

		// Builds the initial free-space root during new-file creation. It records
		// the bootstrap metadata pool before the file is exposed to any reader.
		[[nodiscard]] Superblock initializeNew(const Superblock &initial, PhysicalSlotId bootstrapPoolSlot,
														PhysicalSlotId emergencyReserveSlot = {});

		// Publishes one physical-slot state transition as a new metadata generation.
		[[nodiscard]] Superblock
		setAllocatedAndPublish(SuperblockFile &bootstrap, const Superblock &current, PhysicalSlotId slot,
							   bool allocated, LogSequenceNumber checkpointLsn,
							   BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

		// Reserves the lowest clear bit in the inclusive slot range. metadata pool
		// slots are themselves durable allocated bits and are skipped structurally.
		[[nodiscard]] std::optional<AllocatorReservation>
		reserveFirstFreeAndPublish(SuperblockFile &bootstrap, const Superblock &current, PhysicalSlotId firstSlot,
								   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
								   BootstrapPublishFailpoint failpoint = BootstrapPublishFailpoint::BPF_NONE);

		// Adds one slot transition to an unpublished multi-root COW batch.
		void prepareSetAllocated(MetadataWriteBatch &batch, PhysicalSlotId slot, bool allocated);
		[[nodiscard]] std::optional<PhysicalSlotId>
		prepareReserveFirstFree(MetadataWriteBatch &batch, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot);

	private:
		MetadataPageFile &pages_;
		MetadataPageAllocator &allocator_;
	};

} // namespace graph::storage
