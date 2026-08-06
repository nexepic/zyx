/**
 * @file NativeScalarIndexMutationPlanner.hpp
 * @brief Native Entity-page scalar-index mutation planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/schema/IndexDefinition.hpp"

namespace graph::storage {

	class BTreePageStore;
	class NativeGraphWriteTransaction;
	class RelocatableMetadataStore;
	class MetadataSnapshotPin;
	struct PropertyCollectionRoot;

	// The native property root keeps compact key tokens, while schema definitions
	// retain human-readable key names. The backend supplies a snapshot-aware
	// resolver so index planning never combines names from a later generation
	// with an earlier Entity or Property image.
	using NativePropertyKeyTokenResolver =
			std::function<std::optional<uint32_t>(const MetadataSnapshotPin &, std::string_view)>;

	// Plans the scalar derived state for the native Entity-page representation.
	// It intentionally handles only LABEL, relationship-type, PROPERTY, and
	// COMPOSITE indexes. Vector indexes have a distinct base/delta lifecycle and
	// must be staged by the Stage 10 vector writer rather than as scalar entries.
	class NativeScalarIndexMutationPlanner final {
	public:
		NativeScalarIndexMutationPlanner(RelocatableMetadataStore &metadata, BTreePageStore &indexes,
										 std::vector<IndexDefinition> definitions,
										 NativePropertyKeyTokenResolver propertyKeyTokens);

		// Adds or removes the label entries belonging to one Node. The caller must
		// pass the complete old or new canonical label set, not a delta.
		void stageNodeLabels(NativeGraphWriteTransaction &transaction, uint64_t nodeId,
							 std::span<const uint64_t> labels, bool erase) const;
		// `IDK_LABEL` is the historical schema name for the relationship-type
		// index. Its key is the named relationship type and its value is EdgeId.
		void stageEdgeType(NativeGraphWriteTransaction &transaction, uint64_t edgeId, uint64_t typeId,
						   bool erase) const;
		// Adds or removes every scalar Property/COMPOSITE entry implied by one
		// complete owner-keyed Property root. `nodeLabels` is required only for
		// Node partial indexes and is ignored for Edge roots.
		void stageProperties(NativeGraphWriteTransaction &transaction, IndexDefinitionEntityKind ownerKind,
							 uint64_t ownerId, std::span<const uint64_t> nodeLabels, const PropertyCollectionRoot &root,
							 bool erase) const;
		// UNIQUE and NODE KEY validation supplies canonical Property bytes rather
		// than a lossy hash. The support-tree codec adds its bounded digest and
		// EntityId suffix while preserving collision verification at read time.
		void stageConstraintSupportUpsert(NativeGraphWriteTransaction &transaction, uint64_t constraintId,
										  std::span<const std::byte> canonicalKey, uint64_t entityId) const;
		void stageConstraintSupportErase(NativeGraphWriteTransaction &transaction, uint64_t constraintId,
										 std::span<const std::byte> canonicalKey, uint64_t entityId) const;

		[[nodiscard]] const std::vector<IndexDefinition> &definitions() const { return definitions_; }

	private:
		[[nodiscard]] bool hasPublishedRoot(const MetadataSnapshotPin &snapshot, uint64_t indexId) const;
		[[nodiscard]] std::optional<uint32_t> resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																 std::string_view name) const;

		RelocatableMetadataStore &metadata_;
		BTreePageStore &indexes_;
		std::vector<IndexDefinition> definitions_;
		NativePropertyKeyTokenResolver property_key_tokens_;
	};

} // namespace graph::storage
