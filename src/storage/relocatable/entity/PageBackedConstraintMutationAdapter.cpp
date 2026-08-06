/**
 * @file PageBackedConstraintMutationAdapter.cpp
 * @brief Native Entity-page adapter for logical constraint validation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void insertProperties(const MetadataSnapshotPin &snapshot,
							  const std::vector<PropertyCollectionRootEntry> &entries,
							  const PageBackedConstraintPropertyKeyResolver &resolve,
							  ConstraintValidationEntity &result) {
			result.properties.reserve(entries.size());
			for (const PropertyCollectionRootEntry &entry: entries) {
				const std::string name = resolve(snapshot, entry.key_token);
				if (name.empty() || !result.properties.emplace(name, entry.value).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "page-backed constraint adapter resolved an empty or duplicate Property key");
				}
			}
		}

	} // namespace

	PageBackedConstraintMutationAdapter::PageBackedConstraintMutationAdapter(
			const MetadataSnapshotPin &snapshot, PageBackedGraphReadAdapter &graph, BTreePageStore &indexes,
			RelocatableMetadataStore &metadata, PageBackedConstraintPropertyKeyResolver propertyKeys,
			PageBackedConstraintNodeLabelTokenResolver nodeLabelTokens,
			PageBackedConstraintRelationshipTypeTokenResolver relationshipTypeTokens) :
		snapshot_(&snapshot), graph_(&graph), indexes_(&indexes), metadata_(&metadata),
		property_keys_(std::move(propertyKeys)), node_label_tokens_(std::move(nodeLabelTokens)),
		relationship_type_tokens_(std::move(relationshipTypeTokens)) {
		if (!snapshot.isValid() || !indexes.usesMetadataStore(metadata) || !property_keys_ || !node_label_tokens_ ||
			!relationship_type_tokens_) {
			throw std::invalid_argument(
					"page-backed constraint adapter requires one active snapshot and local name/index resolvers");
		}
	}

	ConstraintValidationEntity PageBackedConstraintMutationAdapter::materialize(LogicalGraphMutationEntityKind kind,
																				uint64_t entityId) const {
		if (entityId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "page-backed constraint adapter requires a nonzero logical Entity ID");
		}
		ConstraintValidationEntity result;
		result.kind = kind;
		result.stable_id = entityId;
		if (kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
			const PageBackedGraphNode node = graph_->readNode(*snapshot_, entityId);
			result.schema_tokens = node.label_token_ids;
			insertProperties(*snapshot_, node.properties, property_keys_, result);
		} else if (kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
			const PageBackedGraphEdge edge = graph_->readEdge(*snapshot_, entityId);
			result.schema_tokens = {edge.type_id};
			insertProperties(*snapshot_, edge.properties, property_keys_, result);
		} else {
			throw std::invalid_argument("page-backed constraint adapter has an unknown Entity kind");
		}
		return result;
	}

	void PageBackedConstraintMutationAdapter::visit(LogicalGraphMutationEntityKind kind, size_t maximum,
													const ConstraintValidationEntityVisitor &visitor) const {
		if (maximum == 0U || maximum == (std::numeric_limits<size_t>::max)() || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "page-backed constraint adapter requires a finite positive scan bound and visitor");
		}
		size_t visited = 0U;
		const auto consume = [this, &visited, maximum, &visitor, kind](uint64_t entityId) {
			if (visited == maximum) {
				fail(FormatError::FERR_INVALID_STATE,
					 "page-backed constraint adapter scan exceeded its caller-supplied Entity bound");
			}
			visitor(materialize(kind, entityId));
			++visited;
			return true;
		};
		// Pass one additional record to distinguish an exact bound from overflow;
		// EntityPageStore itself remains responsible for directory ordering checks.
		const size_t nativeBound = maximum + 1U;
		if (kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
			static_cast<void>(
					graph_->visitNodesUntil(*snapshot_, nativeBound, [&consume](const PageBackedGraphNode &node) {
						return consume(node.node_id);
					}));
		} else if (kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
			static_cast<void>(
					graph_->visitEdgesUntil(*snapshot_, nativeBound, [&consume](const PageBackedGraphEdge &edge) {
						return consume(edge.edge_id);
					}));
		} else {
			throw std::invalid_argument("page-backed constraint adapter has an unknown Entity scan kind");
		}
	}

	ConstraintMutationValidationAdapter PageBackedConstraintMutationAdapter::makeAdapter() const {
		ConstraintMutationValidationAdapter result;
		result.read_persisted_entity = [this](LogicalGraphMutationEntityKind kind, uint64_t entityId) {
			return materialize(kind, entityId);
		};
		result.visit_persisted_entities = [this](LogicalGraphMutationEntityKind kind, size_t maximum,
												 const ConstraintValidationEntityVisitor &visitor) {
			visit(kind, maximum, visitor);
		};
		result.resolve_node_label_token = [this](std::string_view name) {
			const uint64_t token = node_label_tokens_(*snapshot_, name);
			if (token == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "page-backed constraint adapter resolved a zero Node-label Token");
			}
			return token;
		};
		result.resolve_relationship_type_token = [this](std::string_view name) {
			const uint64_t token = relationship_type_tokens_(*snapshot_, name);
			if (token == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "page-backed constraint adapter resolved a zero relationship-type Token");
			}
			return token;
		};
		result.has_support_tree = [this](uint64_t constraintId) {
			return metadata_->findIndexRootOptional(*snapshot_, constraintId).has_value();
		};
		result.scan_support_tree = [this](uint64_t constraintId, std::span<const std::byte> lowerBound,
										  size_t maximum) {
			const BTreeIndex support(constraintId, *indexes_, *metadata_);
			return support.scan(*snapshot_, lowerBound, maximum);
		};
		return result;
	}

} // namespace graph::storage
