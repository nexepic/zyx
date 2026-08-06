/**
 * @file ScalarIndexTreeReachabilityReclaimer.hpp
 * @brief Generation-fenced reclamation of obsolete public scalar-index trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class SystemCatalog;
	class VectorManifestStore;

	// Each limit applies to one maintenance publication. Callers resume with a
	// fresh scan after any bounded run; a corrupted tree never turns cleanup into
	// an unbounded traversal or allocation.
	struct ScalarIndexTreeReclaimOptions {
		size_t maximum_index_definitions = 0U;
		size_t maximum_constraint_definitions = 0U;
		size_t maximum_vector_manifests = 0U;
		size_t maximum_index_roots = 0U;
		size_t maximum_tree_pages = 0U;
		size_t maximum_retirements = 0U;
	};

	struct ScalarIndexTreeReclaimReport {
		Generation scanned_generation{};
		Generation published_generation{};
		uint64_t index_definitions = 0U;
		uint64_t constraint_definitions = 0U;
		uint64_t index_roots_scanned = 0U;
		uint64_t active_roots_retained = 0U;
		uint64_t visible_vector_roots_retained = 0U;
		uint64_t active_constraint_support_roots_retained = 0U;
		uint64_t metadata_roots_retained = 0U;
		uint64_t dropped_index_roots_retired = 0U;
		uint64_t orphaned_constraint_support_roots_retired = 0U;
		uint64_t index_tree_pages_retired = 0U;

		constexpr bool operator==(const ScalarIndexTreeReclaimReport &) const = default;
	};

	// Public scalar Index roots become reclaimable only after their logical
	// definition is absent from the snapshot-pinned System Catalog. Vector
	// indexes share this public root namespace for their Node-to-Vector mapping;
	// a visible Manifest fences that root until the Manifest itself is retired.
	class ScalarIndexTreeReachabilityReclaimer final {
	public:
		// All logical definitions are read from one snapshot-pinned SystemCatalog
		// root; no registry or tombstone tree participates in reachability.
		ScalarIndexTreeReachabilityReclaimer(BTreePageStore &trees, SystemCatalog &catalog,
													 VectorManifestStore &vectorManifests, RelocatableMetadataStore &metadata);

		[[nodiscard]] ScalarIndexTreeReclaimReport
		reclaim(const ScalarIndexTreeReclaimOptions &options, LogSequenceNumber checkpointLsn,
				const MetadataPublishOptions &publishOptions = {});

	private:
		BTreePageStore &trees_;
		SystemCatalog &catalog_;
		VectorManifestStore &vector_manifests_;
		RelocatableMetadataStore &metadata_;
	};

} // namespace graph::storage
