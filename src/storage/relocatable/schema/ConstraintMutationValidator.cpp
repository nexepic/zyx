/**
 * @file ConstraintMutationValidator.cpp
 * @brief Storage-format-independent logical graph constraint validation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	namespace {

		using SupportReference = std::pair<uint8_t, uint64_t>;
		constexpr uint64_t UNTYPED_RELATIONSHIP_TYPE_TOKEN = 0U;

		struct EntityFinalState {
			ConstraintValidationEntity entity;
			ConstraintSupportTreeEntityReferenceKind support_reference_kind =
					ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE;
			uint64_t support_reference_value = 0U;
		};

		struct FastEntityState {
			bool existed_before = false;
			bool exists_after = false;
			EntityFinalState before;
			EntityFinalState after;
		};

		[[noreturn]] void failFormat(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool requiresSupportTree(const ConstraintDefinition &definition) {
			return definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
				   definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		}

		[[nodiscard]] ConstraintSupportTreeEntityReferenceKind
		persistedReferenceKind(LogicalGraphMutationEntityKind kind) {
			return kind == LogicalGraphMutationEntityKind::LGMK_NODE
						   ? ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE
						   : ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_EDGE;
		}

		[[nodiscard]] SupportReference supportReference(const LogicalGraphMutationEntityReference &reference) {
			switch (reference.reference_kind) {
				case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
					return {static_cast<uint8_t>(persistedReferenceKind(reference.entity_kind)), reference.value};
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
					return {static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE),
							reference.value};
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
					return {static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_EDGE),
							reference.value};
			}
			throw std::invalid_argument("relocatable constraint validation has an unknown entity reference kind");
		}

		[[nodiscard]] bool isPersistedReference(SupportReference reference) {
			return reference.first ==
						   static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE) ||
				   reference.first ==
						   static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_EDGE);
		}

		[[nodiscard]] std::unordered_map<std::string, PropertyValue>
		requestProperties(const std::vector<LogicalGraphWriteProperty> &entries) {
			std::unordered_map<std::string, PropertyValue> properties;
			properties.reserve(entries.size());
			for (const LogicalGraphWriteProperty &entry: entries) {
				if (entry.key.empty() || !properties.emplace(entry.key, entry.value).second) {
					throw std::invalid_argument(
							"relocatable constraint validation requires nonempty unique logical Property keys");
				}
			}
			return properties;
		}

		[[nodiscard]] std::vector<uint64_t>
		requestLabels(const std::vector<std::string> &labels,
					  const std::function<uint64_t(std::string_view)> &resolveLabelToken) {
			std::vector<uint64_t> tokens;
			tokens.reserve(labels.size());
			for (const std::string &label: labels) {
				if (label.empty()) {
					throw std::invalid_argument("relocatable constraint validation requires nonempty logical labels");
				}
				const uint64_t token = resolveLabelToken(label);
				if (token == 0U) {
					failFormat(FormatError::FERR_INVALID_STATE,
							   "relocatable constraint validation resolved a zero schema Token");
				}
				tokens.push_back(token);
			}
			std::ranges::sort(tokens);
			tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
			return tokens;
		}

		[[nodiscard]] uint64_t
		requestEdgeType(std::string_view type,
						const std::function<uint64_t(std::string_view)> &resolveRelationshipTypeToken) {
			if (type.empty())
				return UNTYPED_RELATIONSHIP_TYPE_TOKEN;
			const uint64_t token = resolveRelationshipTypeToken(type);
			if (token == 0U) {
				failFormat(FormatError::FERR_INVALID_STATE,
						   "relocatable constraint validation resolved a zero relationship-type Token");
			}
			return token;
		}

		[[nodiscard]] bool matchesScope(const EntityFinalState &state, const ConstraintDefinition &definition) {
			const LogicalGraphMutationEntityKind expected =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
							? LogicalGraphMutationEntityKind::LGMK_NODE
							: LogicalGraphMutationEntityKind::LGMK_EDGE;
			return state.entity.kind == expected &&
				   std::ranges::find(state.entity.schema_tokens, definition.schema_token_id) !=
						   state.entity.schema_tokens.end();
		}

		[[nodiscard]] std::optional<std::vector<std::byte>> keyFor(const EntityFinalState &state,
																   const ConstraintDefinition &definition) {
			if (!matchesScope(state, definition))
				return std::nullopt;
			std::vector<PropertyValue> values;
			values.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const auto value = state.entity.properties.find(name);
				if (value == state.entity.properties.end() || value->second.getType() == PropertyType::NULL_TYPE) {
					return std::nullopt;
				}
				values.push_back(value->second);
			}
			return ConstraintSupportTree::canonicalKey(values);
		}

		[[nodiscard]] bool hasExpectedPropertyType(const PropertyValue &value,
												   ConstraintDefinitionPropertyType expected) {
			switch (expected) {
				case ConstraintDefinitionPropertyType::CDPT_BOOLEAN:
					return value.getType() == PropertyType::BOOLEAN;
				case ConstraintDefinitionPropertyType::CDPT_INTEGER:
					return value.getType() == PropertyType::INTEGER;
				case ConstraintDefinitionPropertyType::CDPT_FLOAT:
					return value.getType() == PropertyType::DOUBLE;
				case ConstraintDefinitionPropertyType::CDPT_STRING:
					return value.getType() == PropertyType::STRING;
				case ConstraintDefinitionPropertyType::CDPT_LIST:
					return value.getType() == PropertyType::LIST;
				case ConstraintDefinitionPropertyType::CDPT_MAP:
					return value.getType() == PropertyType::MAP;
				case ConstraintDefinitionPropertyType::CDPT_NONE:
					break;
			}
			return false;
		}

		void validateState(const EntityFinalState &state, const ConstraintDefinition &definition,
						   std::set<std::vector<std::byte>> *uniqueKeys) {
			if (!matchesScope(state, definition))
				return;
			std::vector<const PropertyValue *> values;
			values.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const auto found = state.entity.properties.find(name);
				values.push_back(found == state.entity.properties.end() ? nullptr : &found->second);
			}
            ConstraintMutationValidator::validatePropertyValues(definition, values);
            const bool missingOrNull = std::ranges::any_of(values, [](const PropertyValue *value) {
                return !value || value->getType() == PropertyType::NULL_TYPE;
            });
			if (uniqueKeys == nullptr || missingOrNull)
				return;
			std::vector<std::byte> key;
			for (const PropertyValue *value: values)
				CanonicalPropertyValueCodec::append(key, *value);
			if (!uniqueKeys->insert(std::move(key)).second) {
				throw std::runtime_error("Constraint violation: '" + definition.name + "' has a duplicate key");
			}
		}

	} // namespace

    void ConstraintMutationValidator::validatePropertyValues(const ConstraintDefinition &definition,
        std::span<const PropertyValue *const> values) {
        if (values.empty() || values.size() != definition.property_names.size())
            throw std::invalid_argument("constraint property view has the wrong component count");
			const bool missingOrNull = std::ranges::any_of(values, [](const PropertyValue *value) {
				return value == nullptr || value->getType() == PropertyType::NULL_TYPE;
			});
			if ((definition.kind == ConstraintDefinitionKind::CDK_NOT_NULL ||
				 definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) &&
				missingOrNull) {
				throw std::runtime_error("Constraint violation: '" + definition.name +
										 "' requires non-null Properties");
			}
			if (definition.kind == ConstraintDefinitionKind::CDK_PROPERTY_TYPE && !missingOrNull &&
				!hasExpectedPropertyType(*values.front(), definition.property_type)) {
				throw std::runtime_error("Constraint violation: '" + definition.name +
										 "' has an invalid Property type");
			}

    }

	ConstraintMutationValidator::ConstraintMutationValidator(ConstraintMutationValidationAdapter adapter,
															 ConstraintMutationValidatorOptions options) :
		adapter_(std::move(adapter)), options_(options) {
		if (!adapter_.read_persisted_entity || !adapter_.visit_persisted_entities ||
			!adapter_.resolve_node_label_token || !adapter_.resolve_relationship_type_token ||
			!adapter_.has_support_tree || !adapter_.scan_support_tree || options_.maximum_entity_scan == 0U) {
			throw std::invalid_argument("constraint mutation validator requires complete bounded snapshot callbacks");
		}
	}

	std::vector<ConstraintSupportTreeMutationPlan>
	ConstraintMutationValidator::validate(std::span<const ConstraintDefinition> definitions,
										  const LogicalGraphMutationBatch &batch,
										  std::span<const ConstraintDefinition> additionalDefinitions) const {
		if (definitions.empty() && additionalDefinitions.empty())
			return {};
		if (definitions.size() > (std::numeric_limits<size_t>::max)() - additionalDefinitions.size()) {
			failFormat(FormatError::FERR_ARITHMETIC_OVERFLOW,
					   "relocatable constraint validation definition count overflows its bound");
		}
		utils::OwnedStdVector<ConstraintDefinition> allDefinitions;
		allDefinitions.reserve(definitions.size() + additionalDefinitions.size());
		for (const auto &definition : definitions) allDefinitions.push_back(definition);
		for (const auto &definition : additionalDefinitions) allDefinitions.push_back(definition);

		const bool canUseAffectedEntityPlanner = additionalDefinitions.empty();
		if (canUseAffectedEntityPlanner) {
			std::map<SupportReference, FastEntityState> changed;
			const auto loadPersisted = [this, &changed](const SupportReference &reference) -> FastEntityState & {
				auto found = changed.find(reference);
				if (found != changed.end())
					return found->second;
				if (!isPersistedReference(reference)) {
					throw std::invalid_argument(
							"relocatable constraint validation cannot load a created entity as persisted");
				}
				const auto kind =
						reference.first == static_cast<uint8_t>(
												   ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE)
								? LogicalGraphMutationEntityKind::LGMK_NODE
								: LogicalGraphMutationEntityKind::LGMK_EDGE;
				ConstraintValidationEntity persisted = adapter_.read_persisted_entity(kind, reference.second);
				if (persisted.kind != kind || persisted.stable_id != reference.second) {
					failFormat(FormatError::FERR_INVALID_STATE,
							   "relocatable constraint validation adapter returned a mismatched persisted Entity");
				}
				EntityFinalState state{std::move(persisted),
									   static_cast<ConstraintSupportTreeEntityReferenceKind>(reference.first),
									   reference.second};
				return changed.emplace(reference, FastEntityState{true, true, state, std::move(state)}).first->second;
			};
			for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
				EntityFinalState state;
				state.entity.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
				state.support_reference_kind = ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE;
				state.support_reference_value = index;
				state.entity.schema_tokens =
						requestLabels(batch.node_creates[index].labels, adapter_.resolve_node_label_token);
				state.entity.properties = requestProperties(batch.node_creates[index].properties);
				changed.emplace(SupportReference{static_cast<uint8_t>(state.support_reference_kind), index},
								FastEntityState{false, true, {}, std::move(state)});
			}
			for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
				EntityFinalState state;
				state.entity.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
				state.support_reference_kind = ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_EDGE;
				state.support_reference_value = index;
				state.entity.schema_tokens = {
						requestEdgeType(batch.edge_creates[index].type, adapter_.resolve_relationship_type_token)};
				state.entity.properties = requestProperties(batch.edge_creates[index].properties);
				changed.emplace(SupportReference{static_cast<uint8_t>(state.support_reference_kind), index},
								FastEntityState{false, true, {}, std::move(state)});
			}
			const auto stateFor = [&changed, &loadPersisted](
										  const LogicalGraphMutationEntityReference &reference) -> FastEntityState & {
				const SupportReference key = supportReference(reference);
				if (isPersistedReference(key))
					return loadPersisted(key);
				auto found = changed.find(key);
				if (found == changed.end()) {
					throw std::invalid_argument(
							"relocatable constraint validation references an unknown created entity");
				}
				return found->second;
			};
			for (const LogicalGraphPropertyMutationRequest &mutation: batch.property_replacements) {
				FastEntityState &state = stateFor(mutation.entity);
				if (!state.exists_after) {
					throw std::invalid_argument(
							"relocatable constraint validation replaces Properties on a deleted entity");
				}
				state.after.entity.properties = requestProperties(mutation.properties);
			}
			for (const LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
				FastEntityState &state = stateFor(mutation.node);
				if (!state.exists_after || state.after.entity.kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
					throw std::invalid_argument("relocatable constraint validation replaces labels on an invalid Node");
				}
				state.after.entity.schema_tokens = requestLabels(mutation.labels, adapter_.resolve_node_label_token);
			}
			for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes) {
				loadPersisted({static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_EDGE),
							   request.edge_id})
						.exists_after = false;
			}
			for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
				loadPersisted({static_cast<uint8_t>(ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE),
							   request.node_id})
						.exists_after = false;
			}

			std::vector<ConstraintSupportTreeMutationPlan> plans;
			for (const ConstraintDefinition &definition: allDefinitions) {
				if (!requiresSupportTree(definition)) {
					for (const auto &[reference, state]: changed) {
						static_cast<void>(reference);
						if (state.exists_after)
							validateState(state.after, definition, nullptr);
					}
					continue;
				}
				if (!adapter_.has_support_tree(definition.constraint_id)) {
					failFormat(FormatError::FERR_INVALID_STATE,
							   "relocatable active uniqueness constraint has no durable support-tree root");
				}
				struct KeyTransition {
					std::optional<std::vector<std::byte>> old_key;
					std::optional<std::vector<std::byte>> new_key;
				};
				std::map<SupportReference, KeyTransition> transitions;
				std::map<uint64_t, std::optional<std::vector<std::byte>>> persistedFinalKeys;
				for (const auto &[reference, state]: changed) {
					if (state.existed_before)
						transitions[reference].old_key = keyFor(state.before, definition);
					if (state.exists_after) {
						const std::optional<std::vector<std::byte>> finalKey = keyFor(state.after, definition);
						if (definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY &&
							matchesScope(state.after, definition) && !finalKey.has_value()) {
							throw std::runtime_error("Constraint violation: '" + definition.name +
													 "' requires non-null Properties");
						}
						transitions[reference].new_key = finalKey;
					}
					if (isPersistedReference(reference)) {
						persistedFinalKeys.emplace(reference.second, transitions[reference].new_key);
					}
				}
				std::map<std::vector<std::byte>, SupportReference> batchOwners;
				for (const auto &[reference, transition]: transitions) {
					if (transition.new_key.has_value() && transition.old_key != transition.new_key &&
						!batchOwners.emplace(*transition.new_key, reference).second) {
						throw std::runtime_error("Constraint violation: '" + definition.name + "' has a duplicate key");
					}
				}
				for (const auto &[reference, transition]: transitions) {
					if (!transition.new_key.has_value() || transition.old_key == transition.new_key)
						continue;
					const utils::OwnedStdVector<BTreePageEntry> candidates = adapter_.scan_support_tree(
							definition.constraint_id, ConstraintSupportTree::lowerBound(*transition.new_key),
							options_.maximum_support_collisions + 1U);
					size_t collisionCount = 0U;
					for (const BTreePageEntry &candidate: candidates) {
						if (!ConstraintSupportTree::matchesDigest(candidate.key, *transition.new_key))
							break;
						if (++collisionCount > options_.maximum_support_collisions) {
							failFormat(FormatError::FERR_INVALID_STATE,
									   "relocatable uniqueness validation exceeded its support-tree collision bound");
						}
						const uint64_t candidateId = ConstraintSupportTree::entryEntityId(candidate.key);
						if (candidate.value_or_child != candidateId) {
							failFormat(FormatError::FERR_INVALID_STATE, "relocatable uniqueness validation found a "
																		"support-tree entry with a mismatched value");
						}
						if (isPersistedReference(reference) && reference.second == candidateId) {
							// A changed key may only be represented by its old support entry before publication.
							failFormat(FormatError::FERR_INVALID_STATE,
										   "relocatable uniqueness validation found a duplicate support-tree entry for "
										   "the mutated Entity");
						}
						const auto changedCandidate = persistedFinalKeys.find(candidateId);
						if (changedCandidate != persistedFinalKeys.end()) {
							if (!changedCandidate->second.has_value() ||
								*changedCandidate->second != *transition.new_key)
								continue;
						} else {
							const LogicalGraphMutationEntityKind kind =
									definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
											? LogicalGraphMutationEntityKind::LGMK_NODE
											: LogicalGraphMutationEntityKind::LGMK_EDGE;
							EntityFinalState current{adapter_.read_persisted_entity(kind, candidateId),
													 persistedReferenceKind(kind), candidateId};
							const auto candidateKey = current.entity.kind == kind && current.entity.stable_id == candidateId
								? keyFor(current, definition) : std::nullopt;
							if (current.entity.kind != kind || current.entity.stable_id != candidateId ||
								!candidateKey.has_value()) {
								failFormat(FormatError::FERR_INVALID_STATE,
										   "relocatable uniqueness validation found a stale support-tree entry");
							}
							if (*candidateKey != *transition.new_key)
								continue;
						}
						throw std::runtime_error("Constraint violation: '" + definition.name + "' has a duplicate key");
					}
				}
				for (auto &[reference, transition]: transitions) {
					if (transition.old_key == transition.new_key)
						continue;
					plans.push_back({definition.constraint_id,
									 static_cast<ConstraintSupportTreeEntityReferenceKind>(reference.first),
									 reference.second, std::move(transition.old_key), std::move(transition.new_key)});
				}
			}
			return plans;
		}

		std::vector<EntityFinalState> nodes;
		std::vector<EntityFinalState> edges;
		std::unordered_map<uint64_t, size_t> persistedNodes;
		std::unordered_map<uint64_t, size_t> persistedEdges;
		const auto visit = [this](LogicalGraphMutationEntityKind kind, std::vector<EntityFinalState> &states,
								  std::unordered_map<uint64_t, size_t> &positions) {
			adapter_.visit_persisted_entities(
					kind, options_.maximum_entity_scan,
					[&states, &positions, kind](const ConstraintValidationEntity &entity) {
						if (entity.kind != kind || entity.stable_id == 0U ||
							!positions.emplace(entity.stable_id, states.size()).second) {
							failFormat(FormatError::FERR_INVALID_STATE,
									   "relocatable constraint validation adapter returned an invalid entity scan");
						}
						states.push_back({entity, persistedReferenceKind(kind), entity.stable_id});
					});
		};
		visit(LogicalGraphMutationEntityKind::LGMK_NODE, nodes, persistedNodes);
		visit(LogicalGraphMutationEntityKind::LGMK_EDGE, edges, persistedEdges);
		const std::vector<EntityFinalState> originalNodes = nodes;
		const std::vector<EntityFinalState> originalEdges = edges;

		std::vector<size_t> createdNodes;
		std::vector<size_t> createdEdges;
		for (const LogicalGraphNodeCreateRequest &request: batch.node_creates) {
			EntityFinalState state;
			state.entity.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
			state.support_reference_kind = ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE;
			state.support_reference_value = createdNodes.size();
			state.entity.schema_tokens = requestLabels(request.labels, adapter_.resolve_node_label_token);
			state.entity.properties = requestProperties(request.properties);
			createdNodes.push_back(nodes.size());
			nodes.push_back(std::move(state));
		}
		for (const LogicalGraphBatchEdgeCreateRequest &request: batch.edge_creates) {
			EntityFinalState state;
			state.entity.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
			state.support_reference_kind = ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_EDGE;
			state.support_reference_value = createdEdges.size();
			state.entity.schema_tokens = {requestEdgeType(request.type, adapter_.resolve_relationship_type_token)};
			state.entity.properties = requestProperties(request.properties);
			createdEdges.push_back(edges.size());
			edges.push_back(std::move(state));
		}
		const auto resolveTarget = [&nodes, &edges, &persistedNodes, &persistedEdges, &createdNodes, &createdEdges](
										   const LogicalGraphMutationEntityReference &reference) -> EntityFinalState & {
			switch (reference.reference_kind) {
				case LogicalGraphMutationReferenceKind::LGRK_PERSISTED: {
					auto &positions = reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE
											  ? persistedNodes
											  : persistedEdges;
					auto &states = reference.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE ? nodes : edges;
					const auto found = positions.find(reference.value);
					if (found == positions.end())
						throw std::runtime_error("logical constraint target entity does not exist");
					return states[found->second];
				}
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= createdNodes.size()) {
						throw std::invalid_argument("logical constraint has an invalid created Node reference");
					}
					return nodes[createdNodes[static_cast<size_t>(reference.value)]];
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
						reference.value >= createdEdges.size()) {
						throw std::invalid_argument("logical constraint has an invalid created Edge reference");
					}
					return edges[createdEdges[static_cast<size_t>(reference.value)]];
			}
			throw std::invalid_argument("logical constraint has an unknown entity reference kind");
		};
		for (const LogicalGraphPropertyMutationRequest &mutation: batch.property_replacements) {
			resolveTarget(mutation.entity).entity.properties = requestProperties(mutation.properties);
		}
		for (const LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
			if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument("logical constraint label mutation requires a Node");
			}
			resolveTarget(mutation.node).entity.schema_tokens =
					requestLabels(mutation.labels, adapter_.resolve_node_label_token);
		}
		const auto erasePersisted = [](std::vector<EntityFinalState> &states,
									   const std::unordered_set<uint64_t> &deleted) {
			states.erase(std::remove_if(states.begin(), states.end(),
										[&deleted](const EntityFinalState &state) {
											return state.entity.stable_id != 0U &&
												   deleted.contains(state.entity.stable_id);
										}),
						 states.end());
		};
		std::unordered_set<uint64_t> deletedNodeIds;
		std::unordered_set<uint64_t> deletedEdgeIds;
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes)
			deletedNodeIds.insert(request.node_id);
		for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes)
			deletedEdgeIds.insert(request.edge_id);
		erasePersisted(nodes, deletedNodeIds);
		erasePersisted(edges, deletedEdgeIds);

		for (const ConstraintDefinition &definition: allDefinitions) {
			std::set<std::vector<std::byte>> uniqueKeys;
			const bool uniqueness = requiresSupportTree(definition);
			const auto &states = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE ? nodes : edges;
			for (const EntityFinalState &state: states)
				validateState(state, definition, uniqueness ? &uniqueKeys : nullptr);
		}

		std::set<uint64_t> additionalIds;
		for (const ConstraintDefinition &definition: additionalDefinitions)
			additionalIds.insert(definition.constraint_id);
		std::vector<ConstraintSupportTreeMutationPlan> plans;
		for (const ConstraintDefinition &definition: allDefinitions) {
			if (!requiresSupportTree(definition) || additionalIds.contains(definition.constraint_id))
				continue;
			if (!adapter_.has_support_tree(definition.constraint_id)) {
				failFormat(FormatError::FERR_INVALID_STATE,
						   "relocatable active uniqueness constraint has no durable support-tree root");
			}
			struct KeyTransition {
				std::optional<std::vector<std::byte>> old_key;
				std::optional<std::vector<std::byte>> new_key;
			};
			std::map<SupportReference, KeyTransition> transitions;
			const auto &original =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE ? originalNodes : originalEdges;
			const auto &final = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE ? nodes : edges;
			for (const EntityFinalState &state: original) {
				if (const auto key = keyFor(state, definition); key.has_value()) {
					transitions[{static_cast<uint8_t>(state.support_reference_kind), state.support_reference_value}]
							.old_key = *key;
				}
			}
			for (const EntityFinalState &state: final) {
				if (const auto key = keyFor(state, definition); key.has_value()) {
					transitions[{static_cast<uint8_t>(state.support_reference_kind), state.support_reference_value}]
							.new_key = *key;
				}
			}
			for (auto &[reference, transition]: transitions) {
				if (transition.old_key == transition.new_key)
					continue;
				plans.push_back({definition.constraint_id,
								 static_cast<ConstraintSupportTreeEntityReferenceKind>(reference.first),
								 reference.second, std::move(transition.old_key), std::move(transition.new_key)});
			}
		}
		return plans;
	}

} // namespace graph::storage
