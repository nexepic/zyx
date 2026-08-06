/**
 * @file StateRecordStore.hpp
 * @brief Durable State record placement through the dedicated State Directory.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/StateRecord.hpp"

namespace graph::storage {

	struct StateRecordStoreOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t page_owner_id = 0;
	};

	class StateRecordStore final {
	public:
		StateRecordStore(SlottedPagePool &pages, RelocatableMetadataStore &metadata);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata && pages_.usesMetadataStore(metadata);
		}

		[[nodiscard]] StateLocation ensure(const StateRecord &record, const StateRecordStoreOptions &options);
		[[nodiscard]] StateRecord read(const MetadataSnapshotPin &snapshot, uint64_t stateId) const;

	private:
		SlottedPagePool &pages_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
