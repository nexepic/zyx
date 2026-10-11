/**
 * @file OfflineVectorBaseRebuild.hpp
 * @brief Atomic base-generation rebuild for PQ, adjacency, and Vector delta state.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"

namespace graph::storage {

	struct OfflineVectorBaseRebuildOptions {
		OfflineVectorPqRebuildOptions pq;
		OfflineVectorAdjacencyRebuildOptions adjacency;
		// Bounds retirement planning for the active delta tree. This is separate
		// from the entry threshold because a damaged tree can occupy more pages
		// than its semantic entry count suggests.
		size_t maximum_delta_mapping_pages = 0U;
		// Bounds validation of the semantic delta inventory before it is folded
		// into the newly built base generation. This is the overlay hard cap, not
		// the lower threshold that merely schedules maintenance.
		size_t maximum_delta_overlay_entries = 0U;

	};

	struct OfflineVectorBaseRebuildResult {
		Superblock superblock;
		VectorManifest manifest;
		uint64_t vector_count = 0U;
		VectorAdjacencyBuildWork adjacency_build_work;
		bool reused_codebook = false;
		bool already_published = false;

		constexpr bool operator==(const OfflineVectorBaseRebuildResult &) const = default;
	};

	// Builds every derived Vector structure from one canonical semantic source
	// snapshot. It stages all candidate frames privately and publishes exactly one
	// replacement Manifest that advances PQ and adjacency together while clearing
	// the delta mapping root.
	class OfflineVectorBaseRebuild final {
	public:
		[[nodiscard]] static OfflineVectorBaseRebuildResult
		run(BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
			VectorCodebookStore &codebooks, VectorManifestStore &manifests, RelocatableMetadataStore &metadata,
			uint64_t indexId, std::span<const DeterministicPqTrainingRecord> records,
			const OfflineVectorBaseRebuildOptions &options, const CancellationCheckpoint *cancellation = nullptr);
	};

} // namespace graph::storage
