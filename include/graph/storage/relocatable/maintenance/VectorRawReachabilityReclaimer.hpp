/**
 * @file VectorRawReachabilityReclaimer.hpp
 * @brief Snapshot-safe reclamation of Raw Vector pages after logical index removal.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class SystemCatalog;
	class VectorManifestStore;

	// All bounds apply to one maintenance invocation. A caller retries from a
	// fresh pinned generation when a larger cleanup set remains.
	struct VectorRawReachabilityReclaimOptions {
		size_t maximum_index_definitions = 0U;
		size_t maximum_vector_manifests = 0U;
		size_t maximum_raw_pools = 0U;
		size_t maximum_raw_pages = 0U;
		size_t maximum_vector_locations = 0U;
		size_t maximum_retirements = 0U;
	};

	struct VectorRawReachabilityReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t index_definitions = 0U;
		uint64_t vector_manifests = 0U;
		uint64_t raw_pools_scanned = 0U;
		uint64_t active_raw_pools_retained = 0U;
		uint64_t stale_manifest_raw_pools_retained = 0U;
		uint64_t raw_pages_scanned = 0U;
		uint64_t vector_locations_scanned = 0U;
		uint64_t raw_pages_retired = 0U;
		uint64_t vector_locations_erased = 0U;
		uint64_t raw_pools_retired = 0U;
		uint64_t raw_pools_released = 0U;

		constexpr bool operator==(const VectorRawReachabilityReclaimReport &) const = default;
	};

	// Logical DROP removes the schema definition first. This reclaimer runs only
	// after stale Manifests have been retired; otherwise it deliberately retains
	// the Raw pool so pinned snapshots keep every Manifest dependency resolvable.
	class VectorRawReachabilityReclaimer final {
	public:
		VectorRawReachabilityReclaimer(VectorPageStore &pages, VectorManifestStore &manifests,
									   SystemCatalog &catalog, RelocatableMetadataStore &metadata);

		[[nodiscard]] VectorRawReachabilityReclaimReport reclaim(const VectorRawReachabilityReclaimOptions &options,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &publishOptions = {});

	private:
		VectorPageStore &pages_;
		VectorManifestStore &manifests_;
		SystemCatalog &catalog_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
