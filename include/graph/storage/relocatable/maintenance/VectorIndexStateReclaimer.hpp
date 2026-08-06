/**
 * @file VectorIndexStateReclaimer.hpp
 * @brief Ordered lifecycle retirement for dropped relocatable Vector indexes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/VectorCodebookReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/VectorPagePoolReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/VectorRawReachabilityReclaimer.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class SystemCatalog;
	class VectorAdjacencyPageStore;
	class VectorManifestStore;

	struct VectorManifestReachabilityReclaimOptions {
		size_t maximum_index_definitions = 0U;
		size_t maximum_vector_manifests = 0U;
	};

	struct VectorManifestReachabilityReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t index_definitions = 0U;
		uint64_t manifests_scanned = 0U;
		uint64_t active_manifests_retained = 0U;
		uint64_t dropped_manifests_retired = 0U;

		constexpr bool operator==(const VectorManifestReachabilityReclaimReport &) const = default;
	};

	struct VectorIndexStateReclaimOptions {
		VectorManifestReachabilityReclaimOptions manifests;
		VectorRawReachabilityReclaimOptions raw;
		DerivedObjectReachabilityReclaimOptions derived;
		ScalarIndexTreeReclaimOptions mapping_trees;
		VectorCodebookReachabilityReclaimOptions codebooks;
		VectorPagePoolReclaimOptions empty_pools;
	};

	struct VectorIndexStateReclaimReport {
		VectorManifestReachabilityReclaimReport manifests;
		VectorRawReachabilityReclaimReport raw;
		DerivedObjectReachabilityReclaimReport adjacency;
		DerivedObjectReachabilityReclaimReport pq;
		DerivedObjectReachabilityReclaimReport derived_mappings;
		ScalarIndexTreeReclaimReport node_mappings;
		VectorCodebookReachabilityReclaimReport codebooks;
		VectorPagePoolReclaimReport empty_pools;

		constexpr bool operator==(const VectorIndexStateReclaimReport &) const = default;
	};

	// DROP commits only its schema transition. This maintenance job applies the
	// physical dependency order afterwards: Manifest, Raw data, derived data,
	// derived mappings, then the public Node-to-Vector mapping tree. Every phase
	// has its own generation fence, making a crash retry safe without undoing the
	// already durable logical DROP.
	class VectorIndexStateReclaimer final {
	public:
		VectorIndexStateReclaimer(PhysicalSegmentAllocator &segments, SlottedPagePool &slottedPages,
								  BTreePageStore &trees, VectorPageStore &pages, VectorAdjacencyPageStore &adjacencies,
								  VectorManifestStore &manifests, SystemCatalog &catalog,
								  RelocatableMetadataStore &metadata);

		[[nodiscard]] VectorIndexStateReclaimReport reclaim(const VectorIndexStateReclaimOptions &options,
															LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &publishOptions = {});

	private:
		[[nodiscard]] VectorManifestReachabilityReclaimReport
		reclaimManifests(const VectorManifestReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
						 const MetadataPublishOptions &publishOptions);
		[[nodiscard]] LogSequenceNumber nextCheckpointLsn() const;

		PhysicalSegmentAllocator &segments_;
		SlottedPagePool &slotted_pages_;
		BTreePageStore &trees_;
		VectorPageStore &pages_;
		VectorAdjacencyPageStore &adjacencies_;
		VectorManifestStore &manifests_;
		SystemCatalog &catalog_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
