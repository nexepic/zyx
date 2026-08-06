/**
 * @file NativeVectorMutationPlanner.hpp
 * @brief Snapshot-bound semantic Vector delta planning for native Entity pages.
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
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	class MetadataSnapshotPin;
	struct PropertyCollectionRoot;

	// Prefixes keep enum members safe when Windows SDK headers are present.
	enum class NativeVectorMutationKind : uint8_t {
		NVMK_ERASE = 1,
		NVMK_INSERT = 2,
	};

	// This is semantic input for the native Vector base/delta writer. An ERASE
	// retires the two semantic mapping entries; an INSERT receives a new logical
	// VectorId only inside the enclosing metadata publication.
	struct NativeVectorMutation {
		NativeVectorMutationKind kind = NativeVectorMutationKind::NVMK_ERASE;
		uint64_t index_id = 0U;
		uint64_t node_id = 0U;
		uint32_t dimensions = 0U;
		VectorDistanceMetric metric = VectorDistanceMetric::VDM_L2;
		std::vector<std::byte> encoded_bfloat16;

		bool operator==(const NativeVectorMutation &) const = default;
	};

	using NativeVectorPropertyKeyTokenResolver =
			std::function<std::optional<uint32_t>(const MetadataSnapshotPin &, std::string_view)>;

	struct NativeVectorMutationPlannerOptions {
		size_t maximum_definitions = 0U;
		size_t maximum_mutations = 0U;
	};

	// Computes the complete semantic difference for one Node's old and final
	// images. It deliberately has no physical-store dependency: the later Vector
	// writer owns VectorId reservation, page placement, and atomic publication.
	class NativeVectorMutationPlanner final {
	public:
		NativeVectorMutationPlanner(std::vector<IndexDefinition> definitions,
									NativeVectorPropertyKeyTokenResolver propertyKeyTokens,
									NativeVectorMutationPlannerOptions options);

		[[nodiscard]] std::vector<NativeVectorMutation>
		planNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId, std::span<const uint64_t> oldLabels,
				 const PropertyCollectionRoot *oldProperties, std::span<const uint64_t> newLabels,
				 const PropertyCollectionRoot *newProperties) const;

	private:
		[[nodiscard]] std::optional<uint32_t> resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																 std::string_view name) const;

		std::vector<IndexDefinition> definitions_;
		NativeVectorPropertyKeyTokenResolver property_key_tokens_;
		NativeVectorMutationPlannerOptions options_;
	};

} // namespace graph::storage
