/**
 * @file NativeStreamedGraphFinalPublication.hpp
 * @brief Metadata-owned input contract for a streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <optional>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPrivatePreparation.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationIndexPlanner.hpp"

namespace graph::storage {
	// The final coordinator accepts only an allocator/Catalog-managed scratch
	// manifest. `open()` is deliberately writer-callback scoped: it binds every
	// payload read to the already fenced metadata snapshot and never opens a
	// second snapshot while the metadata writer is active.
	struct NativeStreamedGraphDurableScratchSource {
		NativeBulkRunStore *store = nullptr;
		NativeBulkRunFragmentLocation manifest_location{};
		uint16_t maximum_manifest_depth = 0U;

		[[nodiscard]] bool isValidFor(const RelocatableMetadataStore &metadata) const noexcept {
			return store != nullptr && store->usesMetadataStore(metadata) && manifest_location.isValid() &&
				   maximum_manifest_depth != 0U;
		}

		[[nodiscard]] NativeBulkEdgeRecordSource open(const MetadataSnapshotPin &snapshot) const {
			if (store == nullptr || !manifest_location.isValid() || maximum_manifest_depth == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "streamed Edge final publication requires a durable scratch manifest");
			}
			return nativeBulkEdgeRecordSource(
					manifest_location, maximum_manifest_depth,
					[this, &snapshot](const NativeBulkRunFragmentLocation &location) {
						return store->readOpaquePayload(snapshot, location);
					});
		}
	};

	// This contains only sealed-source capabilities and scalar placement bounds.
	// The metadata store assigns every public PageId and SegmentId under its
	// writer fence; callers cannot smuggle a raw file offset into graph state.
	struct NativeStreamedGraphFinalPublicationRequest {
		Generation expected_metadata_generation{};
		NativeGraphLogicalIdReservation edge_id_reservation;
		NativeStreamedGraphDurableScratchSource durable_source;
		NativeStreamedGraphPublicationPlan publication_plan;
		NativeStreamedGraphCandidateLayout candidate_layout;
		NativeGraphPrivateSegmentPlan private_candidate_plan;
		std::optional<PhysicalSlotRange> private_source_tail;
		uint64_t entity_pool_owner_id = 0U;
		uint64_t property_pool_owner_id = 0U;
		PhysicalSlotId first_entity_output_slot{};
		PhysicalSlotId last_entity_output_slot{};
		PhysicalSlotId first_property_output_slot{};
		PhysicalSlotId last_property_output_slot{};
		// Shared LargeValue pools are selected only from these writer-validated
		// ranges. The coordinator may append to authenticated current pools first,
		// then reserves fresh pools here when capacity is exhausted.
		PhysicalSlotId first_large_value_data_output_slot{};
		PhysicalSlotId last_large_value_data_output_slot{};
		PhysicalSlotId first_large_value_manifest_output_slot{};
		PhysicalSlotId last_large_value_manifest_output_slot{};
		NativeStreamedGraphPrivatePreparationOptions preparation;
		NativeStreamedGraphPublicationIndexPlanningOptions index_planning;
	};

} // namespace graph::storage
