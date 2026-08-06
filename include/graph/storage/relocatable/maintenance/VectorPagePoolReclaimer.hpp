/**
 * @file VectorPagePoolReclaimer.hpp
 * @brief Snapshot-safe release of empty typed pools owned by Vector state.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class SystemCatalog;

	struct VectorPagePoolReclaimOptions {
		size_t maximum_index_definitions = 0U;
		size_t maximum_catalog_entries = 0U;
		size_t maximum_page_locations = 0U;
		size_t maximum_retire_queue_entries = 0U;
		size_t maximum_pools = 0U;
	};

	struct VectorPagePoolReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t index_definitions = 0U;
		uint64_t catalog_entries_scanned = 0U;
		uint64_t page_locations_scanned = 0U;
		uint64_t retire_queue_entries_scanned = 0U;
		uint64_t relocated_pools_retained = 0U;
		uint64_t nonempty_pools_retained = 0U;
		uint64_t empty_pools_retired = 0U;
		uint64_t retired_pools_released = 0U;

		constexpr bool operator==(const VectorPagePoolReclaimReport &) const = default;
	};

	// Raw pools are handled by VectorRawReachabilityReclaimer because it must
	// atomically remove Vector Location entries with the page. This companion
	// reclaimer releases empty Manifest, PQ, adjacency, and mapping-tree pools.
	class VectorPagePoolReclaimer final {
	public:
		VectorPagePoolReclaimer(SlottedPagePool &pages, SystemCatalog &catalog,
								RelocatableMetadataStore &metadata);

		[[nodiscard]] VectorPagePoolReclaimReport reclaim(const VectorPagePoolReclaimOptions &options,
														  LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &publishOptions = {});

	private:
		SlottedPagePool &pages_;
		SystemCatalog &catalog_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
