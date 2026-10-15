/**
 * @file NativeGraphMutationExecutor.hpp
 * @brief Declarative logical-batch execution over native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	class BTreePageStore;
	class MetadataSnapshotPin;
	class NativeGraphWritePlanner;
	class PageBackedGraphReadAdapter;
	class RelocatableMetadataStore;
	class VectorManifestStore;
	class VectorPageStore;

	// All name resolution is snapshot-bound. Schema lookup is domain-aware so a
	// label and relationship type may legally share the same visible name.
	struct NativeGraphMutationTokenResolvers {
		std::function<std::optional<uint64_t>(const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind,
											  std::string_view)>
				schema_token;
		std::function<std::optional<uint32_t>(const MetadataSnapshotPin &, std::string_view)> property_key_token;
		std::function<std::optional<std::string>(const MetadataSnapshotPin &, uint32_t)> property_key_name;
	};

	using NativeGraphMutationIndexDefinitionReader =
			std::function<utils::OwnedStdVector<IndexDefinition>(const MetadataSnapshotPin &)>;
	using NativeGraphMutationConstraintDefinitionReader =
			std::function<utils::OwnedStdVector<ConstraintDefinition>(const MetadataSnapshotPin &)>;
	using NativeGraphMutationFenceValidator = std::function<bool(const Superblock &)>;

	struct NativeGraphMutationExecutorOptions {
		size_t maximum_index_definitions = 0U;
		size_t maximum_constraint_definitions = 0U;
		size_t maximum_constraint_entity_scan = 0U;
		size_t maximum_constraint_support_collisions = 0U;
		// Bounds one logical graph batch's per-Vector-index semantic work. The
		// writer translates it into bounded mapping, Raw-page, and Manifest work
		// before the single metadata publication point.
		size_t maximum_vector_records = 0U;
		size_t maximum_vector_pages = 0U;
		// The threshold schedules a base rebuild while the hard cap rejects a
		// mutation only when a maintainer has fallen too far behind. Keeping these
		// distinct preserves availability without accepting unbounded overlays.
		size_t vector_delta_rebuild_threshold = 0U;
		size_t maximum_vector_delta_overlay_entries = 0U;
		size_t maximum_vector_delta_pages = 0U;
	};

	struct NativeGraphMutationExecutionResult {
		LogicalGraphMutationBatchResult result;
		NativeGraphWriteCommitResult commit;

		[[nodiscard]] constexpr bool committed() const { return commit.committed(); }
	};

	// Executes one already-tokenizable logical batch through the native Entity
	// Page transaction. Graph records, owner-keyed Properties, scalar indexes,
	// and constraint support trees publish in one generation. A supplied System
	// Catalog allocates new names in that same publication. Vector semantic
	// transitions use the same native transaction, so graph records, mapping
	// trees, Raw locations, and the immutable Manifest cannot become visible
	// independently.
	class NativeGraphMutationExecutor final {
	public:
		NativeGraphMutationExecutor(NativeGraphWritePlanner &writer, PageBackedGraphReadAdapter &graph,
									BTreePageStore &indexes, VectorPageStore &vectorPages,
									VectorManifestStore &vectorManifests, RelocatableMetadataStore &metadata,
									NativeGraphMutationTokenResolvers tokens,
									NativeGraphMutationIndexDefinitionReader indexDefinitions,
									NativeGraphMutationConstraintDefinitionReader constraintDefinitions,
									NativeGraphMutationExecutorOptions options, SystemCatalog *systemCatalog = nullptr);

		[[nodiscard]] NativeGraphMutationExecutionResult
		execute(const LogicalGraphMutationBatch &batch,
				const NativeGraphMutationFenceValidator &expectedFence = {},
				const LogicalGraphMutationReadControl &readControl = {}) const;

		// Internal native patch entry point. Sources remain tokenized in the
		// transaction snapshot; unchanged fields need no name resolution.
		[[nodiscard]] NativeGraphMutationExecutionResult executeWithPropertyPatches(
				const LogicalGraphMutationBatch &batch,
				std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
				const NativeGraphMutationFenceValidator &expectedFence,
				const LogicalGraphMutationReadControl &readControl = {}) const;

	private:
		[[nodiscard]] NativeGraphMutationExecutionResult executeImpl(
				const LogicalGraphMutationBatch &batch,
				std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
				const NativeGraphMutationFenceValidator &expectedFence,
				const LogicalGraphMutationReadControl &readControl) const;
		NativeGraphWritePlanner &writer_;
		PageBackedGraphReadAdapter &graph_;
		BTreePageStore &indexes_;
		VectorPageStore &vector_pages_;
		VectorManifestStore &vector_manifests_;
		RelocatableMetadataStore &metadata_;
		NativeGraphMutationTokenResolvers tokens_;
		NativeGraphMutationIndexDefinitionReader index_definitions_;
		NativeGraphMutationConstraintDefinitionReader constraint_definitions_;
		NativeGraphMutationExecutorOptions options_;
		SystemCatalog *system_catalog_ = nullptr;
	};

} // namespace graph::storage
