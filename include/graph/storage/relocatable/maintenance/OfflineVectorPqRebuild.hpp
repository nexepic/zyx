/**
 * @file OfflineVectorPqRebuild.hpp
 * @brief Offline, atomically published construction of one derived PQ Vector generation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/vector/DeterministicProductQuantizer.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	// Every ID names a fresh derived generation, except when a restart proves the
	// already durable object has exactly the same canonical semantic contents.
	struct OfflineVectorPqRebuildOptions {
		DeterministicPqOptions training;
		VectorCodebookWriteRequest codebook;
		PhysicalSlotId codebook_first_slot{};
		PhysicalSlotId codebook_last_slot{};
		VectorPqMaterializationOptions pages;
		uint64_t mapping_index_id = 0;
		VectorPqMappingBuildOptions mapping;
		// This failpoint hook applies to the single visibility switch. Codebook,
		// PQ, mapping, and Manifest candidates remain unreachable until it commits.
		MetadataPublishOptions manifest_publish_options;
	};

	struct OfflineVectorPqRebuildResult {
		Superblock superblock;
		VectorManifest manifest;
		uint64_t vector_count = 0;
		bool reused_codebook = false;
		bool reused_mapping = false;
		bool already_published = false;

		bool operator==(const OfflineVectorPqRebuildResult &) const = default;
	};

	// Rebuilds only acceleration data. Exact VectorId -> raw page/slot lookup
	// remains in the raw Vector Location Directory and is never changed here.
	// The caller must provide an offline/exclusive source view for the duration
	// of this operation. Codebook extents, PQ pages, mapping root, and Manifest
	// replacement are made reachable by one Superblock switch.
	class OfflineVectorPqRebuild final {
	public:
		[[nodiscard]] static OfflineVectorPqRebuildResult
		run(BTreePageStore &trees, VectorPageStore &vectors, VectorCodebookStore &codebooks,
			VectorManifestStore &manifests, RelocatableMetadataStore &metadata, uint64_t indexId,
			std::span<const DeterministicPqTrainingRecord> records, const OfflineVectorPqRebuildOptions &options);
	};

} // namespace graph::storage
