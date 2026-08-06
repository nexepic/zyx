/**
 * @file VectorCodebookReachabilityReclaimer.hpp
 * @brief Manifest-rooted reclamation for immutable Vector Codebook extents.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class VectorManifestStore;

	struct VectorCodebookReachabilityReclaimOptions {
		size_t maximum_vector_manifests = 0U;
		size_t maximum_extent_locations = 0U;
		size_t maximum_catalog_entries = 0U;
		size_t maximum_retire_queue_entries = 0U;
		size_t maximum_retirements = 0U;
	};

	struct VectorCodebookReachabilityReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t vector_manifests = 0U;
		uint64_t extent_locations = 0U;
		uint64_t catalog_entries = 0U;
		uint64_t retire_queue_entries = 0U;
		uint64_t reachable_codebooks = 0U;
		uint64_t reachable_codebook_extents = 0U;
		uint64_t relocated_codebooks_retained = 0U;
		uint64_t codebooks_retired = 0U;
		uint64_t codebook_extents_retired = 0U;
		uint64_t codebook_extents_released = 0U;

		constexpr bool operator==(const VectorCodebookReachabilityReclaimReport &) const = default;
	};

	class VectorCodebookReachabilityReclaimer final {
	public:
		VectorCodebookReachabilityReclaimer(PhysicalSegmentAllocator &segments, VectorManifestStore &manifests,
											RelocatableMetadataStore &metadata);

		[[nodiscard]] VectorCodebookReachabilityReclaimReport
		reclaim(const VectorCodebookReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &publishOptions = {});

	private:
		PhysicalSegmentAllocator &segments_;
		VectorManifestStore &manifests_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
