/**
 * @file PageBackedConstraintMutationAdapter.cpp
 * @brief Native Entity-page adapter for logical constraint validation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"

#include <limits>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		template<typename Entries>
		void insertProperties(const MetadataSnapshotPin &snapshot,
							  Entries &entries,
							  const PageBackedConstraintPropertyKeyResolver &resolve,
							  ConstraintValidationEntity &result) {
			result.properties.reserve(entries.size());
			for (auto &entry: entries) {
				std::string name = resolve(snapshot, entry.key_token);
				if (name.empty()) fail(FormatError::FERR_INVALID_STATE, "page-backed constraint adapter resolved an empty Property key");
				const bool inserted = [&] {
					if constexpr (std::is_const_v<Entries>) return result.properties.emplace(std::move(name), entry.value).second;
					else return result.properties.emplace(std::move(name), std::move(entry.value)).second;
				}();
				if (!inserted) {
					fail(FormatError::FERR_INVALID_STATE,
						 "page-backed constraint adapter resolved an empty or duplicate Property key");
				}
			}
		}

		template<typename Entity>
		[[nodiscard]] ConstraintValidationEntity makeValidationEntity(
				const MetadataSnapshotPin &snapshot, Entity &&entity,
				const PageBackedConstraintPropertyKeyResolver &resolve) {
			ConstraintValidationEntity result;
			if constexpr (std::is_same_v<std::remove_cvref_t<Entity>, PageBackedGraphNode>) {
				result.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
				result.stable_id = entity.node_id;
				if constexpr (std::is_lvalue_reference_v<Entity>) result.schema_tokens = entity.label_token_ids;
				else result.schema_tokens = std::move(entity.label_token_ids);
			} else {
				result.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
				result.stable_id = entity.edge_id;
				result.schema_tokens = {entity.type_id};
			}
			insertProperties(snapshot, entity.properties, resolve, result);
			return result;
		}

	} // namespace
	PageBackedConstraintPropertySelection selectConstraintProperties(
		std::span<const ConstraintDefinition> definitions,
		const std::function<std::optional<uint32_t>(std::string_view)> &resolve) {
		if (!resolve) throw std::invalid_argument("constraint Property selection requires a Token resolver");
		PageBackedConstraintPropertySelection result;
		for (const auto &definition: definitions) {
			auto &tokens = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE ? result.node_tokens : result.edge_tokens;
			for (const auto &name: definition.property_names) {
				const auto token = resolve(name);
				if (!token) continue; // A missing catalog key cannot occur in persisted Property data.
				if (*token == 0U) throw std::invalid_argument("constraint Property selection resolved a zero Token");
				tokens.push_back(*token);
			}
		}
		for (auto *tokens: {&result.node_tokens, &result.edge_tokens}) {
			std::ranges::sort(*tokens);
			tokens->erase(std::unique(tokens->begin(), tokens->end()), tokens->end());
		}
		return result;
	}

	PageBackedConstraintMutationAdapter::PageBackedConstraintMutationAdapter(
			const MetadataSnapshotPin &snapshot, PageBackedGraphReadAdapter &graph, BTreePageStore &indexes,
			RelocatableMetadataStore &metadata, PageBackedConstraintPropertyKeyResolver propertyKeys,
			PageBackedConstraintNodeLabelTokenResolver nodeLabelTokens,
			PageBackedConstraintRelationshipTypeTokenResolver relationshipTypeTokens,
			std::optional<PageBackedConstraintPropertySelection> selection, std::function<void()> checkpoint, std::function<void(size_t)> propertyAdmission) :
		snapshot_(&snapshot), graph_(&graph), indexes_(&indexes), metadata_(&metadata),
		property_keys_(std::move(propertyKeys)), node_label_tokens_(std::move(nodeLabelTokens)),
		relationship_type_tokens_(std::move(relationshipTypeTokens)), selection_(std::move(selection)), checkpoint_(std::move(checkpoint)), property_admission_(std::move(propertyAdmission)) {
		if (!snapshot.isValid() || !indexes.usesMetadataStore(metadata) || !property_keys_ || !node_label_tokens_ ||
			!relationship_type_tokens_) {
			throw std::invalid_argument(
					"page-backed constraint adapter requires one active snapshot and local name/index resolvers");
		}
		if (selection_) {
			for (const auto *tokens: {&selection_->node_tokens, &selection_->edge_tokens}) {
				validateEntityReadShape({.property_tokens = *tokens});
			}
		}
	}

	std::vector<ConstraintValidationEntity> PageBackedConstraintMutationAdapter::materializeSelected(
		LogicalGraphMutationEntityKind kind, std::span<const uint64_t> ids) const {
		if (checkpoint_) checkpoint_();
		std::vector<ConstraintValidationEntity> result(ids.size());
		const bool node = kind == LogicalGraphMutationEntityKind::LGMK_NODE;
		if (!node && kind != LogicalGraphMutationEntityKind::LGMK_EDGE) throw std::invalid_argument("unknown constraint Entity kind");
		if (node) {
			auto labels = graph_->readNodeLabelTokensSorted(*snapshot_, ids, &read_session_);
			if (labels.size() != ids.size()) fail(FormatError::FERR_INVALID_STATE, "constraint Node labels changed cardinality");
			for (size_t i = 0U; i < ids.size(); ++i) result[i].schema_tokens = std::move(labels[i]);
		} else {
			const auto edges = graph_->readEdgeTopologiesSorted(*snapshot_, ids, &read_session_);
			if (edges.size() != ids.size()) fail(FormatError::FERR_INVALID_STATE, "constraint Edge topology changed cardinality");
			for (size_t i = 0U; i < ids.size(); ++i) result[i].schema_tokens = {edges[i].type_id};
		}
		for (size_t i = 0U; i < ids.size(); ++i) { result[i].kind = kind; result[i].stable_id = ids[i]; }
		const auto &tokens = node ? selection_->node_tokens : selection_->edge_tokens;
		if (tokens.empty()) return result;
		auto admission = makeSelectedPropertyBatchAdmission(property_admission_);
		auto columns = node ? graph_->readNodePropertyColumnsSorted(*snapshot_, ids, tokens, &read_session_, admission ? &admission : nullptr)
			: graph_->readEdgePropertyColumnsSorted(*snapshot_, ids, tokens, &read_session_, admission ? &admission : nullptr);
		if (columns.size() != tokens.size()) fail(FormatError::FERR_INVALID_STATE, "constraint Property columns changed cardinality");
		std::unordered_set<std::string> names;
		for (size_t column = 0U; column < tokens.size(); ++column) {
			if (columns[column].size() != ids.size()) fail(FormatError::FERR_INVALID_STATE, "constraint Property column changed cardinality");
			// A newly allocated catalog Token has no name in the pinned base yet,
			// and cannot occur in any persisted row. Leave that field absent.
			if (std::ranges::none_of(columns[column], [](const auto &value) { return value.has_value(); })) continue;
			std::string name = property_keys_(*snapshot_, tokens[column]);
			if (name.empty() || !names.insert(name).second || columns[column].size() != ids.size())
				fail(FormatError::FERR_INVALID_STATE, "constraint selected Property has an invalid key or cardinality");
			for (size_t row = 0U; row < ids.size(); ++row) {
				if (columns[column][row]) result[row].properties.emplace(name, std::move(*columns[column][row]));
			}
		}
		return result;
	}

	ConstraintValidationEntity PageBackedConstraintMutationAdapter::materialize(LogicalGraphMutationEntityKind kind,
																				uint64_t entityId) const {
		if (entityId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "page-backed constraint adapter requires a nonzero logical Entity ID");
		}
		if (selection_) {
			auto selected = materializeSelected(kind, std::array<uint64_t, 1U>{entityId});
			return std::move(selected.front());
		}
		if (kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
			return makeValidationEntity(*snapshot_, graph_->readNode(*snapshot_, entityId), property_keys_);
		}
		if (kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
			return makeValidationEntity(*snapshot_, graph_->readEdge(*snapshot_, entityId), property_keys_);
		}
		throw std::invalid_argument("page-backed constraint adapter has an unknown Entity kind");
	}

	void PageBackedConstraintMutationAdapter::visit(LogicalGraphMutationEntityKind kind, size_t maximum,
													const ConstraintValidationEntityVisitor &visitor) const {
		if (maximum == 0U || maximum == (std::numeric_limits<size_t>::max)() || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "page-backed constraint adapter requires a finite positive scan bound and visitor");
		}
		size_t visited = 0U;
		const auto consume = [this, &visited, maximum, &visitor](const auto &entity) {
			if (visited == maximum) {
				fail(FormatError::FERR_INVALID_STATE,
					 "page-backed constraint adapter scan exceeded its caller-supplied Entity bound");
			}
			if (visited % 128U == 0U && checkpoint_) checkpoint_();
			visitor(makeValidationEntity(*snapshot_, entity, property_keys_));
			++visited;
			return true;
		};
		// Pass one additional record to distinguish an exact bound from overflow;
		// EntityPageStore itself remains responsible for directory ordering checks.
		const size_t nativeBound = maximum + 1U;
		if (selection_) {
			std::vector<uint64_t> pending;
			const size_t capacity = std::min<size_t>(128U, maximum);
			pending.reserve(capacity);
			const auto flush = [&] {
				if (pending.empty()) return;
				for (const auto &entity: materializeSelected(kind, pending)) {
					visitor(entity); ++visited;
				}
				pending.clear();
			};
			const auto collect = [&](const EntityRef &ref) {
				if (visited + pending.size() == maximum) {
					flush();
					fail(FormatError::FERR_INVALID_STATE, "page-backed constraint adapter scan exceeded its caller-supplied Entity bound");
				}
				pending.push_back(ref.entity_id);
				if (pending.size() == capacity) flush();
				return true;
			};
			if (kind == LogicalGraphMutationEntityKind::LGMK_NODE) static_cast<void>(graph_->visitNodeRefsUntil(*snapshot_, nativeBound, collect, &read_session_));
			else if (kind == LogicalGraphMutationEntityKind::LGMK_EDGE) static_cast<void>(graph_->visitEdgeRefsUntil(*snapshot_, nativeBound, collect, &read_session_));
			else throw std::invalid_argument("unknown constraint Entity scan kind");
			flush();
			return;
		}
		if (kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
			static_cast<void>(
					graph_->visitNodesUntil(*snapshot_, nativeBound, [&consume](const PageBackedGraphNode &node) {
						return consume(node);
					}));
		} else if (kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
			static_cast<void>(
					graph_->visitEdgesUntil(*snapshot_, nativeBound, [&consume](const PageBackedGraphEdge &edge) {
						return consume(edge);
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
