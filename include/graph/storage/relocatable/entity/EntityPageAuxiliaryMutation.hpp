/**
 * @file EntityPageAuxiliaryMutation.hpp
 * @brief Preplanned non-Entity frames published with one native Entity transaction.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	// These plans deliberately cover only typed auxiliary frames in an Entity
	// page pool. The native graph writer validates the owner kind and complete
	// source identity before preparing any bytes, so this is not a raw-page write
	// escape hatch for higher layers.
	struct EntityPageAuxiliaryReplacement {
		CatalogEntry expected_pool;
		PageLocation expected_location;
		PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
		LogSequenceNumber expected_owner_lsn{};
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0U;
		bool retire_page = false;
		// Auxiliary Root/Label pages use the same cross-pool COW rule as fixed
		// Entity pages. The PageId remains stable across the route change.
		CatalogEntry replacement_pool;
		std::optional<EntityPageFreshPoolDestination> replacement_new_pool;
	};

	struct EntityPageAuxiliaryAppend {
		CatalogEntry expected_pool;
		PageId page_id{};
		PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0U;
	};

	struct EntityPageAuxiliaryNewPool {
		SegmentId segment_id{};
		uint64_t pool_owner_id = 0U;
		Generation generation{};
		PageId page_id{};
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		PageOwnerKind owner_kind = PageOwnerKind::POK_NONE;
		std::vector<std::byte> payload;
		uint32_t free_bytes = 0U;
	};

	// A LabelSet frame becomes reachable only through this exact Node record.
	// Keeping the Entity Map location, durable slot generation, and replacement
	// record together lets EntityPageStore reject an orphan auxiliary plan before
	// it stages any candidate frames.
	struct EntityPageAuxiliaryNodeUpdateBinding {
		uint64_t node_id = 0U;
		EntityPageLocation expected_location;
		NodeEntityRecord replacement_record;
	};

	// Newly inserted Nodes have no snapshot-visible Entity Map entry yet. Their
	// binding still makes the final fixed record the only publication path to a
	// staged LabelSet frame, while EntityPageStore verifies it against exactly one
	// Node insertion in the same batch.
	struct EntityPageAuxiliaryNodeInsertionBinding {
		uint64_t node_id = 0U;
		NodeEntityRecord inserted_record;
	};

	struct EntityPageAuxiliaryMutationBatch {
		std::vector<EntityPageAuxiliaryReplacement> replacements;
		std::vector<EntityPageAuxiliaryAppend> appends;
		std::vector<EntityPageAuxiliaryNewPool> new_pools;
		std::vector<EntityPageAuxiliaryNodeUpdateBinding> node_update_bindings;
		std::vector<EntityPageAuxiliaryNodeInsertionBinding> node_insertion_bindings;

		[[nodiscard]] bool empty() const {
			return replacements.empty() && appends.empty() && new_pools.empty();
		}
	};

} // namespace graph::storage
