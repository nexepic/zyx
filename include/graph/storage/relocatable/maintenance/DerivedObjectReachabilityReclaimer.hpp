/**
 * @file DerivedObjectReachabilityReclaimer.hpp
 * @brief Conservative reachability reclamation for immutable relocatable derived data.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;

	// Every scan bound is caller-supplied so an offline cleanup job cannot turn a
	// damaged directory into an unbounded memory or CPU walk.
	struct DerivedObjectReachabilityReclaimOptions {
		size_t maximum_vector_manifests = 0;
		size_t maximum_vector_locations = 0;
		size_t maximum_mapping_entries = 0;
		size_t maximum_candidate_pages = 0;
		size_t maximum_retirements = 0;
		size_t maximum_mapping_roots = 0;
		size_t maximum_mapping_tree_pages = 0;
	};

	struct DerivedObjectReachabilityReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t vector_manifests = 0;
		uint64_t vector_locations = 0;
		uint64_t reachable_adjacency_pages = 0;
		uint64_t non_derived_adjacency_pages_retained = 0;
		uint64_t derived_adjacency_pages_retired = 0;
		uint64_t reachable_pq_pages = 0;
		uint64_t derived_pq_pages_retired = 0;
		uint64_t mapping_roots_scanned = 0;
		uint64_t non_derived_mapping_roots_retained = 0;
		uint64_t reachable_mapping_roots = 0;
		uint64_t derived_mapping_roots_retired = 0;
		uint64_t derived_mapping_tree_pages_retired = 0;

		constexpr bool operator==(const DerivedObjectReachabilityReclaimReport &) const = default;
	};

	// This first classifier intentionally reclaims only high-bit-owned derived
	// adjacency pages. Unknown owner namespaces, PQ frames, mapping trees, Large
	// Objects, and registry trees are retained until each has an explicit root
	// classifier. Failing closed is more important than reclaiming every byte.
	class DerivedObjectReachabilityReclaimer final {
	public:
		DerivedObjectReachabilityReclaimer(BTreePageStore &trees, VectorPageStore &vectors,
										   VectorAdjacencyPageStore &adjacencies, VectorManifestStore &manifests,
										   RelocatableMetadataStore &metadata);

		[[nodiscard]] DerivedObjectReachabilityReclaimReport
		reclaim(const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &publishOptions = {});
		// PQ cleanup is a separate generation-fenced publication because an
		// adjacency-only Manifest remains query-correct while a staged PQ cache is
		// removed. Non-raw Vector Location entries are rejected as malformed.
		[[nodiscard]] DerivedObjectReachabilityReclaimReport
		reclaimPq(const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
				  const MetadataPublishOptions &publishOptions = {});
		// Removes only typed PQ/adjacency mapping trees that no visible Manifest
		// names. The matching Index Root entry and every reachable tree page are
		// retired in one generation-fenced publication.
		[[nodiscard]] DerivedObjectReachabilityReclaimReport
		reclaimMappingTrees(const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
									const MetadataPublishOptions &publishOptions = {});

	private:
		BTreePageStore &trees_;
		VectorPageStore &vectors_;
		VectorAdjacencyPageStore &adjacencies_;
		VectorManifestStore &manifests_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
