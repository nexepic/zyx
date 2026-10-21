/**
 * @file NativeScalarIndexMutationPlanner.hpp
 * @brief Native Entity-page scalar-index mutation planning.
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
#include <unordered_map>
#include <vector>

#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/core/PropertyTypes.hpp"

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
										 utils::OwnedStdVector<IndexDefinition> definitions,
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
		// Compare canonical derived keys, including partial-index membership. Only
		// changed entries are staged; complete roots remain authoritative inputs.
		void stagePropertyDelta(NativeGraphWriteTransaction &transaction, IndexDefinitionEntityKind ownerKind,
			uint64_t ownerId, std::span<const uint64_t> oldLabels, const PropertyCollectionRoot &oldRoot,
			std::span<const uint64_t> newLabels, const PropertyCollectionRoot &newRoot) const;
        // Read-only preparation for metadata-owned atomic coordinators. Keys
        // borrow temporary canonical buffers only during each callback. This
        // grants no publication authority and never stages transaction state.
        using PropertyDeltaVisitor = std::function<void(uint64_t indexId, std::span<const std::byte> key,
            uint64_t ownerId, bool erase)>;
        void visitPropertyDelta(const MetadataSnapshotPin &snapshot, IndexDefinitionEntityKind ownerKind,
            uint64_t ownerId, std::span<const uint64_t> oldLabels, const PropertyCollectionRoot &oldRoot,
            std::span<const uint64_t> newLabels, const PropertyCollectionRoot &newRoot,
            const PropertyDeltaVisitor &visitor) const;
		// UNIQUE and NODE KEY validation supplies canonical Property bytes rather
		// than a lossy hash. The support-tree codec adds its bounded digest and
		// EntityId suffix while preserving collision verification at read time.
		void stageConstraintSupportUpsert(NativeGraphWriteTransaction &transaction, uint64_t constraintId,
										  std::span<const std::byte> canonicalKey, uint64_t entityId) const;
		void stageConstraintSupportErase(NativeGraphWriteTransaction &transaction, uint64_t constraintId,
										 std::span<const std::byte> canonicalKey, uint64_t entityId) const;

		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const noexcept { return &metadata_ == &metadata; }

		[[nodiscard]] const std::vector<IndexDefinition> &definitions() const { return definitions_; }

	private:
        friend class RelocatableMetadataStore;
        using PropertyValueLookup = std::function<const PropertyValue *(uint32_t)>;
        void visitPropertyDeltaValues(const MetadataSnapshotPin &snapshot, IndexDefinitionEntityKind ownerKind,
            uint64_t ownerId, std::span<const uint64_t> oldLabels, const PropertyValueLookup &oldValues,
            std::span<const uint64_t> newLabels, const PropertyValueLookup &newValues,
            const PropertyDeltaVisitor &visitor) const;
        [[nodiscard]] std::optional<std::vector<std::byte>> propertyEntryKeyValues(const MetadataSnapshotPin &snapshot,
            const IndexDefinition &definition, uint64_t ownerId, std::span<const uint64_t> nodeLabels,
            const PropertyValueLookup &values) const;

		[[nodiscard]] bool hasPublishedRoot(const MetadataSnapshotPin &snapshot, uint64_t indexId) const;
		[[nodiscard]] std::optional<uint32_t> resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																 std::string_view name) const;

		[[nodiscard]] std::optional<std::vector<std::byte>> propertyEntryKey(const MetadataSnapshotPin &snapshot,
			const IndexDefinition &definition, uint64_t ownerId, std::span<const uint64_t> nodeLabels,
			const PropertyCollectionRoot &root) const;

		RelocatableMetadataStore &metadata_;
		BTreePageStore &indexes_;
		utils::OwnedStdVector<IndexDefinition> definitions_;
		NativePropertyKeyTokenResolver property_key_tokens_;
		// Index-root presence is immutable for a pinned metadata generation. The
		// planner may stage thousands of entities against one snapshot, so retain
		// the result of each root lookup and invalidate it on generation changes.
		mutable std::unordered_map<uint64_t, bool, std::hash<uint64_t>, std::equal_to<uint64_t>,
            utils::OwnedPmrAllocator<std::pair<const uint64_t, bool>>> published_root_cache_;
		mutable uint64_t published_root_cache_database_id_ = 0U;
		mutable uint64_t published_root_cache_generation_ = 0U;
	};

} // namespace graph::storage
