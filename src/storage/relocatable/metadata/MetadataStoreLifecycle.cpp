/**
 * @file MetadataStoreLifecycle.cpp
 * @brief Bootstrap lifecycle for a relocatable single-file metadata store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataStoreLifecycle.hpp"

#include <stdexcept>
#include <string>

#include "graph/storage/StorageFormat.hpp"
#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"

namespace graph::storage {

	namespace {

		void requireStorageIO(const std::shared_ptr<StorageIO> &io) {
			if (!io) {
				throw std::invalid_argument("relocatable metadata store requires database storage");
			}
		}

		void requireEmptyDataFile(const StorageIO &io) {
			const StorageFormatInfo format = StorageFormatProbe::inspect(io);
			if (!format.isEmpty()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable initialization refuses a non-empty " +
											  std::string(StorageFormatProbe::describe(format.kind)) + " database file");
			}
		}

		void requireRelocatableDataFile(const StorageIO &io) {
			const StorageFormatInfo format = StorageFormatProbe::inspect(io);
			if (!format.isRelocatable()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable metadata open refuses a " +
											  std::string(StorageFormatProbe::describe(format.kind)) + " database file");
			}
		}

	} // namespace

	void initializeRelocatableMetadataFile(const std::shared_ptr<StorageIO> &io, const Superblock &initial) {
		requireStorageIO(io);
		requireEmptyDataFile(*io);
		SuperblockFile bootstrap(io);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(io, initial);
		MetadataPagePoolAllocator allocator(io, initial);
		AllocatorDirectory freeSpace(pages, allocator);
		const PhysicalSlotId bootstrapPoolSlot(1U);
		const PhysicalSlotId emergencyReserveSlot(2U);
		Superblock initialized = freeSpace.initializeNew(initial, bootstrapPoolSlot, emergencyReserveSlot);
		const SegmentId bootstrapPoolSegmentId(1U);
		allocator.bindCatalogPoolIdentity(bootstrapPoolSlot, bootstrapPoolSegmentId, initialized.generation,
											 initialized.checkpoint_lsn);
		MetadataEmergencyReserveDescriptor emergencyReserve;
		emergencyReserve.physical_slot = emergencyReserveSlot;
		emergencyReserve.pool_token = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
		emergencyReserve.generation = initialized.generation;
		emergencyReserve.committed_lsn = initialized.checkpoint_lsn;
		allocator.createEmergencyReserve(emergencyReserve);
		initialized.metadata_emergency_reserve = emergencyReserve;
		// SegmentId 1 names the bootstrap pool and the next identity belongs to
		// the bootstrap-addressed reserve. All later pools start after both.
		if (initialized.next_logical_segment_id != 0U &&
			initialized.next_logical_segment_id < RSS_FIRST_ALLOCATABLE_SEGMENT_ID) {
			initialized.next_logical_segment_id = RSS_FIRST_ALLOCATABLE_SEGMENT_ID;
		}
		// A new file has no old generation to preserve, so both Superblocks record
		// the complete generation-one bootstrap ownership before the first open.
		bootstrap.initializeNew(initialized);
	}

	Superblock recoverRelocatableMetadataSuperblock(const std::shared_ptr<StorageIO> &io) {
		requireStorageIO(io);
		requireRelocatableDataFile(*io);
		SuperblockFile bootstrap(io);
		return bootstrap.recover();
	}

} // namespace graph::storage
