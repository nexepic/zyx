/**
 * @file PageBackedConstraintMutationAdapter.hpp
 * @brief Native Entity-page adapter for format-independent constraint validation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"

namespace graph::storage {

	class BTreePageStore;
	class MetadataSnapshotPin;
	class PageBackedGraphReadAdapter;
	class RelocatableMetadataStore;

	using PageBackedConstraintPropertyKeyResolver = std::function<std::string(const MetadataSnapshotPin &, uint32_t)>;
	using PageBackedConstraintNodeLabelTokenResolver =
			std::function<uint64_t(const MetadataSnapshotPin &, std::string_view)>;
	using PageBackedConstraintRelationshipTypeTokenResolver =
			std::function<uint64_t(const MetadataSnapshotPin &, std::string_view)>;

	// Bridges the owner-keyed native Entity/Property view into the logical state
	// required by ConstraintMutationValidator. The adapter is snapshot-bound and
	// never exposes page, slot, or legacy PropertyId state to the validator.
	class PageBackedConstraintMutationAdapter final {
	public:
		PageBackedConstraintMutationAdapter(const MetadataSnapshotPin &snapshot, PageBackedGraphReadAdapter &graph,
											BTreePageStore &indexes, RelocatableMetadataStore &metadata,
											PageBackedConstraintPropertyKeyResolver propertyKeys,
											PageBackedConstraintNodeLabelTokenResolver nodeLabelTokens,
											PageBackedConstraintRelationshipTypeTokenResolver relationshipTypeTokens);

		[[nodiscard]] ConstraintMutationValidationAdapter makeAdapter() const;

	private:
		[[nodiscard]] ConstraintValidationEntity materialize(LogicalGraphMutationEntityKind kind,
															 uint64_t entityId) const;
		void visit(LogicalGraphMutationEntityKind kind, size_t maximum,
				   const ConstraintValidationEntityVisitor &visitor) const;

		const MetadataSnapshotPin *snapshot_ = nullptr;
		PageBackedGraphReadAdapter *graph_ = nullptr;
		BTreePageStore *indexes_ = nullptr;
		RelocatableMetadataStore *metadata_ = nullptr;
		PageBackedConstraintPropertyKeyResolver property_keys_;
		PageBackedConstraintNodeLabelTokenResolver node_label_tokens_;
		PageBackedConstraintRelationshipTypeTokenResolver relationship_type_tokens_;
	};

} // namespace graph::storage
