/**
 * @file OfflineVectorAdjacencyRebuild.hpp
 * @brief Offline, resumable publication of a derived adjacency generation.
 */

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"

namespace graph::storage {

	// The builder choice is not persisted: the published adjacency records and
	// their Manifest generation remain the complete durable contract.
	enum class VectorAdjacencyBuildMode : uint8_t {
		VABM_EXACT = 0,
		VABM_CANDIDATE_PRUNED = 1,
	};

	struct CanonicalVectorRecord {
		uint64_t vector_id = 0;
		std::vector<std::byte> raw_bfloat16;

		bool operator==(const CanonicalVectorRecord &) const = default;
	};

	struct OfflineVectorAdjacencyRebuildOptions {
		// Exact construction is retained as the deterministic small-input baseline.
		size_t maximum_exact_records = 0;
		VectorAdjacencyBuildMode build_mode = VectorAdjacencyBuildMode::VABM_EXACT;
		// Candidate-pruned construction bounds every graph traversal instead of
		// enumerating every other Vector. These limits are required in that mode.
		size_t maximum_candidate_records = 0;
		size_t candidate_search_budget = 0;
		// A candidate is pruned when an accepted neighbor dominates it according
		// to this finite, non-contracting L2 multiplier.
		float pruning_alpha = 1.0F;
		uint16_t maximum_degree = 0;
		Generation adjacency_generation{};
		VectorAdjacencyMaterializationOptions pages;
		uint64_t mapping_index_id = 0;
		VectorAdjacencyMappingBuildOptions mapping;
		// This failpoint hook applies only to the final Manifest visibility switch.
		MetadataPublishOptions manifest_publish_options;
	};

	// Transient structural work counters for an adjacency rebuild. They are not
	// persisted and deliberately exclude wall-clock measurements, whose values
	// depend on the deployment host and belong in the production calibration
	// harness. Candidate counters cover only bounded traversal/pruning work.
	struct VectorAdjacencyBuildWork {
		uint64_t exact_pair_distance_evaluations = 0;
		uint64_t candidate_frontier_pushes = 0;
		uint64_t candidate_frontier_pops = 0;
		uint64_t candidate_frontier_peak = 0;
		uint64_t candidate_prune_calls = 0;
		uint64_t candidate_prune_candidates = 0;
		uint64_t candidate_prune_pair_checks = 0;
		uint64_t candidate_reverse_link_updates = 0;

		bool operator==(const VectorAdjacencyBuildWork &) const = default;
	};

	struct OfflineVectorAdjacencyRebuildResult {
		Superblock superblock;
		VectorManifest manifest;
		uint64_t vector_count = 0;
		VectorAdjacencyBuildWork build_work;
		bool reused_mapping = false;
		bool already_published = false;

		bool operator==(const OfflineVectorAdjacencyRebuildResult &) const = default;
	};

	// The construction phase is deliberately separable from publication. A
	// compound base rebuild uses this deterministic graph output while one
	// metadata transaction publishes PQ, adjacency, and delta retirement.
	struct OfflineVectorAdjacencyPreparedGraph {
		std::vector<VectorAdjacencyMaterializationRecord> records;
		VectorAdjacencyBuildWork build_work;

		bool operator==(const OfflineVectorAdjacencyPreparedGraph &) const = default;
	};

	// Rebuilds an immutable adjacency generation for the Manifest's declared
	// distance metric. Exact raw Vector Location lookups remain authoritative
	// and are revalidated before Manifest commit.
	class OfflineVectorAdjacencyRebuild final {
	public:
		[[nodiscard]] static OfflineVectorAdjacencyPreparedGraph
		buildGraph(VectorDistanceMetric metric, uint32_t dimensions, std::span<const CanonicalVectorRecord> records,
						   const OfflineVectorAdjacencyRebuildOptions &options);
		[[nodiscard]] static OfflineVectorAdjacencyRebuildResult
		run(BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
			VectorManifestStore &manifests, RelocatableMetadataStore &metadata, uint64_t indexId,
			std::span<const CanonicalVectorRecord> records, const OfflineVectorAdjacencyRebuildOptions &options);
	};

} // namespace graph::storage
