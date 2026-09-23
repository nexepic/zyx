/**
 * @file NativeGraphMutationExecutor.cpp
 * @brief Declarative logical-batch execution over native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	namespace {

		struct ResolvedProperties {
			std::vector<PropertyCollectionRootEntry> entries;
		};

		struct ResolvedNodeCreate {
			std::vector<uint64_t> labels;
			ResolvedProperties properties;
		};

		struct PersistedNodeMutation {
			std::optional<std::vector<uint64_t>> labels;
			std::optional<ResolvedProperties> properties;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] std::tuple<uint8_t, uint8_t, uint64_t>
		referenceKey(const LogicalGraphMutationEntityReference &reference) {
			return {static_cast<uint8_t>(reference.entity_kind), static_cast<uint8_t>(reference.reference_kind),
					reference.value};
		}

		void validateReference(const LogicalGraphMutationBatch &batch,
							   const LogicalGraphMutationEntityReference &reference, const char *operation) {
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE &&
				reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) {
				throw std::invalid_argument(std::string(operation) + " has an unknown logical entity kind");
			}
			switch (reference.reference_kind) {
				case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
					if (reference.value == 0U) {
						throw std::invalid_argument(std::string(operation) +
													" requires a nonzero persisted logical ID");
					}
					return;
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= batch.node_creates.size()) {
						throw std::invalid_argument(std::string(operation) +
													" has an invalid created-Node batch reference");
					}
					return;
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
						reference.value >= batch.edge_creates.size()) {
						throw std::invalid_argument(std::string(operation) +
													" has an invalid created-Edge batch reference");
					}
					return;
			}
			throw std::invalid_argument(std::string(operation) + " has an unknown logical reference kind");
		}

		void validateNodeReference(const LogicalGraphMutationBatch &batch,
								   const LogicalGraphMutationEntityReference &reference, const char *operation) {
			validateReference(batch, reference, operation);
			if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument(std::string(operation) + " requires a Node reference");
			}
		}

		void validateCreationIdentity(const LogicalGraphMutationBatch &batch, const char *entityName,
									  std::span<const LogicalGraphCreationIdentityRange> ranges,
									  const std::vector<uint64_t> &ids, bool nodes) {
			if (ids.size() != (nodes ? batch.node_creates.size() : batch.edge_creates.size())) {
				throw std::invalid_argument(std::string("native logical graph mutation has an invalid ") + entityName +
											" creation identity sequence");
			}
			uint64_t previousRangeLast = 0U;
			for (const LogicalGraphCreationIdentityRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					throw std::invalid_argument(std::string("native logical graph mutation has an invalid ") +
												entityName + " creation identity range");
				}
				const uint64_t last = range.first_id + range.count - 1U;
				if (previousRangeLast != 0U && range.first_id <= previousRangeLast) {
					throw std::invalid_argument(std::string("native logical graph mutation has unordered ") +
												entityName + " creation identity ranges");
				}
				previousRangeLast = last;
			}
			if (!ids.empty() && ranges.empty()) {
				throw std::invalid_argument(std::string("native logical graph mutation has an unleased ") + entityName +
											" creation identity");
			}
			uint64_t previous = 0U;
			for (const uint64_t id: ids) {
				if ((previous != 0U && id <= previous) ||
					!std::ranges::any_of(ranges, [id](const LogicalGraphCreationIdentityRange &range) {
						return range.contains(id);
					})) {
					throw std::invalid_argument(std::string("native logical graph mutation has an invalid ") +
												entityName + " creation identity sequence");
				}
				previous = id;
			}
		}

		void validateShape(const LogicalGraphMutationBatch &batch) {
			if (batch.creation_identity_plan.has_value()) {
				const LogicalGraphCreationIdentityPlan &identity = *batch.creation_identity_plan;
				validateCreationIdentity(batch, "Node", identity.node_id_ranges, identity.node_ids, true);
				validateCreationIdentity(batch, "Edge", identity.edge_id_ranges, identity.edge_ids, false);
			}

			std::unordered_set<uint64_t> deletedEdges;
			deletedEdges.reserve(batch.edge_deletes.size());
			for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes) {
				if (request.edge_id == 0U || !deletedEdges.insert(request.edge_id).second) {
					throw std::invalid_argument("native logical graph mutation requires unique nonzero Edge deletes");
				}
			}
			std::unordered_set<uint64_t> deletedNodes;
			deletedNodes.reserve(batch.node_deletes.size());
			for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			if (request.node_id == 0U || !deletedNodes.insert(request.node_id).second ||
				(request.detach && (request.maximum_detach_edges == 0U ||
									request.maximum_detach_edges == std::numeric_limits<size_t>::max()))) {
				throw std::invalid_argument(
						"native logical graph mutation requires unique Node deletes and positive finite detach bounds");
				}
			}
			for (const LogicalGraphBatchEdgeCreateRequest &request: batch.edge_creates) {
				validateNodeReference(batch, request.source, "native logical graph Edge creation");
				validateNodeReference(batch, request.target, "native logical graph Edge creation");
				for (const LogicalGraphMutationEntityReference *endpoint: {&request.source, &request.target}) {
					if (endpoint->reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
						deletedNodes.contains(endpoint->value)) {
						throw std::invalid_argument(
								"native logical graph Edge creation cannot target a Node deleted in the same batch");
					}
				}
			}

			std::set<std::tuple<uint8_t, uint8_t, uint64_t>> propertyTargets;
			for (const LogicalGraphPropertyMutationRequest &request: batch.property_replacements) {
				validateReference(batch, request.entity, "native logical graph Property replacement");
				if (!propertyTargets.insert(referenceKey(request.entity)).second) {
					throw std::invalid_argument(
							"native logical graph mutation has duplicate Property replacement targets");
				}
				if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
					((request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
					  deletedNodes.contains(request.entity.value)) ||
					 (request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
					  deletedEdges.contains(request.entity.value)))) {
					throw std::invalid_argument(
							"native logical graph mutation cannot replace Properties on a deleted entity");
				}
				if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
					!batch.node_creates[static_cast<size_t>(request.entity.value)].properties.empty()) {
					throw std::invalid_argument("native logical graph mutation cannot replace Properties already "
												"supplied for a created Node");
				}
				if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
					!batch.edge_creates[static_cast<size_t>(request.entity.value)].properties.empty()) {
					throw std::invalid_argument("native logical graph mutation cannot replace Properties already "
												"supplied for a created Edge");
				}
			}

			std::set<std::tuple<uint8_t, uint8_t, uint64_t>> labelTargets;
			for (const LogicalGraphNodeLabelMutationRequest &request: batch.node_label_replacements) {
				validateNodeReference(batch, request.node, "native logical graph Node label replacement");
				if (!labelTargets.insert(referenceKey(request.node)).second) {
					throw std::invalid_argument(
							"native logical graph mutation has duplicate Node label replacement targets");
				}
				if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
					deletedNodes.contains(request.node.value)) {
					throw std::invalid_argument(
							"native logical graph mutation cannot replace labels on a deleted Node");
				}
			}
		}

		[[nodiscard]] ResolvedProperties
		resolveProperties(const std::vector<LogicalGraphWriteProperty> &properties,
						  const std::function<std::optional<uint32_t>(std::string_view)> &resolvePropertyKey) {
			std::vector<LogicalGraphWriteProperty> ordered = properties;
			std::ranges::sort(ordered, {}, &LogicalGraphWriteProperty::key);
			for (size_t index = 0U; index < ordered.size(); ++index) {
				if (ordered[index].key.empty() || (index != 0U && ordered[index - 1U].key == ordered[index].key)) {
					throw std::invalid_argument(
							"native logical graph mutation requires nonempty, unique Property keys per entity");
				}
			}
			ResolvedProperties result;
			result.entries.reserve(ordered.size());
			for (const LogicalGraphWriteProperty &property: ordered) {
				const std::optional<uint32_t> token = resolvePropertyKey(property.key);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation could not resolve a Property-key Token in its snapshot");
				}
				result.entries.push_back({*token, property.value});
			}
			std::ranges::sort(result.entries, {}, &PropertyCollectionRootEntry::key_token);
			for (size_t index = 1U; index < result.entries.size(); ++index) {
				if (result.entries[index - 1U].key_token == result.entries[index].key_token) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation resolved distinct Property keys to one Token");
				}
			}
			return result;
		}

		// Edge validation runs before durable ID consumption. Validate the canonical
		// key/token shape without copying or sorting the Property values; the actual
		// materialization below performs that work exactly once.
		void validatePropertyKeysAndTokens(
				const std::vector<LogicalGraphWriteProperty> &properties,
				const std::function<std::optional<uint32_t>(std::string_view)> &resolvePropertyKey) {
			std::unordered_set<std::string_view> names;
			std::unordered_set<uint32_t> tokens;
			names.reserve(properties.size());
			tokens.reserve(properties.size());
			for (const LogicalGraphWriteProperty &property: properties) {
				if (property.key.empty() || !names.emplace(property.key).second) {
					throw std::invalid_argument(
							"native logical graph mutation requires nonempty, unique Property keys per entity");
				}
				const std::optional<uint32_t> token = resolvePropertyKey(property.key);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native logical graph mutation could not resolve a Property-key Token in its snapshot");
				}
				if (!tokens.emplace(*token).second) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native logical graph mutation resolved distinct Property keys to one Token");
				}
			}
		}

		[[nodiscard]] std::vector<uint64_t>
		resolveLabels(const std::vector<std::string> &labels,
					  const std::function<std::optional<uint64_t>(std::string_view)> &resolveSchemaToken) {
			std::vector<std::string> ordered = labels;
			std::ranges::sort(ordered);
			ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
			for (const std::string &label: ordered) {
				if (label.empty()) {
					throw std::invalid_argument("native logical graph mutation requires nonempty label names");
				}
			}
			std::vector<uint64_t> tokens;
			tokens.reserve(ordered.size());
			for (const std::string &label: ordered) {
				const std::optional<uint64_t> token = resolveSchemaToken(label);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation could not resolve a schema Token in its snapshot");
				}
				tokens.push_back(*token);
			}
			std::ranges::sort(tokens);
			if (std::ranges::adjacent_find(tokens) != tokens.end()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native logical graph mutation resolved distinct labels to one schema Token");
			}
			return tokens;
		}

		[[nodiscard]] PropertyCollectionRoot nodeRoot(uint64_t nodeId,
													  std::vector<PropertyCollectionRootEntry> entries) {
			return {nodeId, Node::typeId, std::move(entries)};
		}

		[[nodiscard]] PropertyCollectionRoot edgeRoot(uint64_t edgeId,
													  std::vector<PropertyCollectionRootEntry> entries) {
			return {edgeId, Edge::typeId, std::move(entries)};
		}

		[[nodiscard]] uint64_t resolveEntityId(const LogicalGraphMutationEntityReference &reference,
											   const LogicalGraphMutationBatchResult &result) {
			// Edge endpoints were validated as persisted or batch-created Nodes before
			// execution begins; created Edges cannot be endpoints of another Edge.
			return reference.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED
					   ? reference.value
					   : result.created_node_ids.at(static_cast<size_t>(reference.value));
		}

		[[nodiscard]] uint64_t resolveSupportEntityId(const ConstraintSupportTreeMutationPlan &plan,
														  const LogicalGraphMutationBatchResult &result) {
			switch (plan.entity_reference_kind) {
				case ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE:
				case ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_EDGE:
					return plan.entity_reference_value;
				case ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE:
					return result.created_node_ids.at(static_cast<size_t>(plan.entity_reference_value));
				case ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_EDGE:
					return result.created_edge_ids.at(static_cast<size_t>(plan.entity_reference_value));
			}
			throw std::logic_error("native constraint support plan has an unknown entity reference kind");
		}

		[[nodiscard]] uint64_t checkedCardinalityDelta(uint64_t current, size_t additions, size_t removals,
															 const char *name) {
			if (additions > std::numeric_limits<uint64_t>::max() ||
				removals > std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native graph cardinality mutation exceeds the persisted counter width");
			}
			const uint64_t added = static_cast<uint64_t>(additions);
			const uint64_t removed = static_cast<uint64_t>(removals);
			if (added > std::numeric_limits<uint64_t>::max() - current) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, name);
			}
			const uint64_t expanded = current + added;
			if (removed > expanded) {
				fail(FormatError::FERR_INVALID_STATE, name);
			}
			return expanded - removed;
		}

	} // namespace

	NativeGraphMutationExecutor::NativeGraphMutationExecutor(
			NativeGraphWritePlanner &writer, PageBackedGraphReadAdapter &graph, BTreePageStore &indexes,
			VectorPageStore &vectorPages, VectorManifestStore &vectorManifests, RelocatableMetadataStore &metadata,
			NativeGraphMutationTokenResolvers tokens, NativeGraphMutationIndexDefinitionReader indexDefinitions,
			NativeGraphMutationConstraintDefinitionReader constraintDefinitions,
			NativeGraphMutationExecutorOptions options, SystemCatalog *systemCatalog) :
		writer_(writer), graph_(graph), indexes_(indexes), vector_pages_(vectorPages),
		vector_manifests_(vectorManifests), metadata_(metadata), tokens_(std::move(tokens)),
		index_definitions_(std::move(indexDefinitions)), constraint_definitions_(std::move(constraintDefinitions)),
		options_(options), system_catalog_(systemCatalog) {
		const bool needsResolvers = system_catalog_ == nullptr;
		if (!indexes_.usesMetadataStore(metadata_) || !vector_pages_.usesMetadataStore(metadata_) ||
			!vector_manifests_.usesMetadataStore(metadata_) || !index_definitions_ || !constraint_definitions_ ||
			(needsResolvers && (!tokens_.schema_token || !tokens_.property_key_token || !tokens_.property_key_name)) ||
			(system_catalog_ != nullptr && !system_catalog_->usesMetadataStore(metadata_)) ||
			options_.maximum_index_definitions == 0U || options_.maximum_constraint_definitions == 0U ||
			options_.maximum_constraint_entity_scan == 0U || options_.maximum_constraint_support_collisions == 0U ||
			options_.maximum_vector_records == 0U || options_.maximum_vector_pages == 0U ||
			options_.vector_delta_rebuild_threshold == 0U || options_.maximum_vector_delta_overlay_entries == 0U ||
			options_.vector_delta_rebuild_threshold > options_.maximum_vector_delta_overlay_entries ||
			options_.maximum_vector_delta_pages == 0U ||
			options_.maximum_vector_delta_overlay_entries >
					VectorDeltaMapping::maximumEntriesForPageBudget(options_.maximum_vector_delta_pages)) {
			throw std::invalid_argument("native graph mutation executor requires local stores, one canonical Token "
										"source, and finite bounds");
		}
	}

	NativeGraphMutationExecutionResult
	NativeGraphMutationExecutor::execute(const LogicalGraphMutationBatch &batch,
									 const NativeGraphMutationFenceValidator &expectedFence) const {
		debug::ScopedPerfTimer timer("native_graph.execute");
		validateShape(batch);
		NativeGraphWriteTransaction transaction = writer_.beginTransaction();
		if (expectedFence && !expectedFence(transaction.baseSuperblock())) {
			transaction.rollback();
			throw LogicalGraphMutationConflict(
					"native logical graph mutation batch no longer matches its read snapshot fence");
		}
		if (batch.creation_identity_plan.has_value()) {
			const LogicalGraphCreationIdentityPlan &identity = *batch.creation_identity_plan;
			NativeGraphLogicalIdReservation reservation;
			reservation.node_ranges.reserve(identity.node_id_ranges.size());
			reservation.edge_ranges.reserve(identity.edge_id_ranges.size());
			for (const LogicalGraphCreationIdentityRange &range: identity.node_id_ranges) {
				reservation.node_ranges.push_back({range.first_id, range.count});
			}
			for (const LogicalGraphCreationIdentityRange &range: identity.edge_id_ranges) {
				reservation.edge_ranges.push_back({range.first_id, range.count});
			}
			transaction.adoptLogicalIdReservation(std::move(reservation));
		}
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		std::optional<SystemCatalogWriteSession> catalogSession;
		if (system_catalog_ != nullptr) {
			catalogSession.emplace(system_catalog_->beginWrite(transaction));
		}
		// Token resolution is snapshot-stable for the lifetime of this transaction.
		// Cache both hits and misses so bulk validation and materialization do not
		// repeatedly descend the same catalog/index for every row.
		using SchemaTokenCacheKey = std::pair<SystemCatalogSchemaTokenKind, std::string>;
		std::map<SchemaTokenCacheKey, std::optional<uint64_t>> resolvedSchemaTokens;
		std::map<std::string, std::optional<uint32_t>, std::less<>> resolvedPropertyTokens;
		std::map<std::string, uint32_t, std::less<>> stagedPropertyNames;
		std::map<uint32_t, std::string> stagedPropertyTokens;
		const auto resolveSchemaToken = [this, &snapshot, &catalogSession,
												 &resolvedSchemaTokens](SystemCatalogSchemaTokenKind kind,
																		 std::string_view name) -> std::optional<uint64_t> {
			const SchemaTokenCacheKey cacheKey{kind, std::string(name)};
			const auto cached = resolvedSchemaTokens.find(cacheKey);
			if (cached != resolvedSchemaTokens.end()) {
				return cached->second;
			}
			std::optional<uint64_t> resolved;
			if (catalogSession.has_value()) {
				resolved = catalogSession->ensureSchemaToken(kind, name);
			} else {
				resolved = tokens_.schema_token(snapshot, kind, name);
			}
			resolvedSchemaTokens.emplace(cacheKey, resolved);
			return resolved;
		};
		const auto resolvePropertyKey = [this, &snapshot, &catalogSession, &stagedPropertyNames,
									 &stagedPropertyTokens,
									 &resolvedPropertyTokens](std::string_view name) -> std::optional<uint32_t> {
			const auto cached = resolvedPropertyTokens.find(name);
			if (cached != resolvedPropertyTokens.end()) {
				return cached->second;
			}
			std::optional<uint32_t> resolved;
			if (!catalogSession.has_value()) {
				resolved = tokens_.property_key_token(snapshot, name);
			} else {
				const auto staged = stagedPropertyNames.find(name);
				if (staged != stagedPropertyNames.end()) {
					resolved = staged->second;
				} else {
					const uint32_t token = catalogSession->ensurePropertyToken(name);
					stagedPropertyNames.emplace(name, token);
					stagedPropertyTokens.emplace(token, std::string(name));
					resolved = token;
				}
			}
			resolvedPropertyTokens.emplace(std::string(name), resolved);
			return resolved;
		};
		const auto resolvePropertyName = [this, &snapshot, &catalogSession,
										  &stagedPropertyTokens](uint32_t token) -> std::optional<std::string> {
			const auto staged = stagedPropertyTokens.find(token);
			if (staged != stagedPropertyTokens.end()) {
				return staged->second;
			}
			if (catalogSession.has_value()) {
				return system_catalog_->findPropertyName(snapshot, token);
			}
			return tokens_.property_key_name(snapshot, token);
		};

		std::vector<ResolvedNodeCreate> nodes;
		nodes.reserve(batch.node_creates.size());
		for (const LogicalGraphNodeCreateRequest &request: batch.node_creates) {
			nodes.push_back({resolveLabels(request.labels,
											   [&resolveSchemaToken](std::string_view name) {
												   return resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL,
																		 name);
											   }),
							 resolveProperties(request.properties, resolvePropertyKey)});
		}
		// Validate each Edge before the first graph mutation consumes a durable
		// logical-ID lease. The resolved values are intentionally transient: keeping
		// an all-edge resolved copy was a second O(batch) payload allocation.
		for (const LogicalGraphBatchEdgeCreateRequest &request: batch.edge_creates) {
			if (!request.type.empty()) {
				const std::optional<uint64_t> resolved =
						resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, request.type);
				if (!resolved.has_value() || *resolved == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation could not resolve a relationship-type Token in its snapshot");
				}
			}
			validatePropertyKeysAndTokens(request.properties, resolvePropertyKey);
		}
		std::unordered_map<uint64_t, PersistedNodeMutation> persistedNodes;
		std::unordered_map<uint64_t, ResolvedProperties> persistedEdges;
		// Do not materialize a second full resolved-edge batch. Bulk edge ingest
		// already owns the logical request list; only statement-local property
		// replacements need an additional sparse override map.
		std::unordered_map<uint64_t, ResolvedProperties> createdEdgeProperties;
		for (const LogicalGraphPropertyMutationRequest &request: batch.property_replacements) {
			const ResolvedProperties resolved = resolveProperties(request.properties, resolvePropertyKey);
			if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				if (request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
					persistedNodes[request.entity.value].properties = resolved;
				} else {
					persistedEdges.emplace(request.entity.value, resolved);
				}
			} else if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				nodes.at(static_cast<size_t>(request.entity.value)).properties = resolved;
			} else {
				createdEdgeProperties.insert_or_assign(request.entity.value, resolved);
			}
		}
		for (const LogicalGraphNodeLabelMutationRequest &request: batch.node_label_replacements) {
			const std::vector<uint64_t> resolved =
					resolveLabels(request.labels, [&resolveSchemaToken](std::string_view name) {
						return resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
					});
			if (request.node.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				persistedNodes[request.node.value].labels = resolved;
			} else {
				nodes.at(static_cast<size_t>(request.node.value)).labels = resolved;
			}
		}

		std::unordered_set<uint64_t> deletedEdges;
		deletedEdges.reserve(batch.edge_deletes.size());
		for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes) {
			deletedEdges.insert(request.edge_id);
		}
		// DETACH planning must see the batch's final topology. Stage explicit Edge
		// removals before discovering a Node's remaining incident Edges; otherwise
		// an already deleted Edge can incorrectly consume the detach bound.
		std::unordered_set<uint64_t> topologyDeletedEdges;
		topologyDeletedEdges.reserve(deletedEdges.size());
		for (const uint64_t edgeId: deletedEdges) {
			transaction.eraseEdge(edgeId);
			topologyDeletedEdges.insert(edgeId);
		}
		LogicalGraphMutationBatch constraintBatch = batch;
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			if (!request.detach) {
				continue;
			}
			for (const uint64_t edgeId:
				 transaction.collectIncidentEdgeIds(request.node_id, request.maximum_detach_edges)) {
				if (deletedEdges.insert(edgeId).second) {
					constraintBatch.edge_deletes.push_back({edgeId});
				}
			}
		}
		for (const uint64_t edgeId: deletedEdges) {
			if (topologyDeletedEdges.insert(edgeId).second) {
				transaction.eraseEdge(edgeId);
			}
		}

		const std::vector<IndexDefinition> indexDefinitions = index_definitions_(snapshot);
		const std::vector<ConstraintDefinition> constraintDefinitions = constraint_definitions_(snapshot);
		if (indexDefinitions.size() > options_.maximum_index_definitions ||
			constraintDefinitions.size() > options_.maximum_constraint_definitions) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native logical graph mutation definition reader exceeded its configured bound");
		}
		NativeScalarIndexMutationPlanner scalar(
				metadata_, indexes_, indexDefinitions,
				[&resolvePropertyKey](const MetadataSnapshotPin &, std::string_view name) {
					return resolvePropertyKey(name);
				});
		const bool hasVectorDefinition = std::ranges::any_of(indexDefinitions, [](const IndexDefinition &definition) {
			return definition.kind == IndexDefinitionKind::IDK_VECTOR;
		});
		std::optional<NativeVectorMutationPlanner> vector;
		if (hasVectorDefinition) {
			if (indexDefinitions.size() > std::numeric_limits<size_t>::max() / 2U) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native logical graph Vector definition count overflows its per-Node work bound");
			}
			vector.emplace(
					indexDefinitions,
					[&resolvePropertyKey](const MetadataSnapshotPin &, std::string_view name) {
						return resolvePropertyKey(name);
					},
					NativeVectorMutationPlannerOptions{options_.maximum_index_definitions,
													   indexDefinitions.size() * 2U});
			// A mapping root and its Manifest are one published Vector state. Reject
			// every logical write when either half is missing, including writes that
			// do not themselves alter an embedding property.
			for (const IndexDefinition &definition: indexDefinitions) {
				if (definition.kind != IndexDefinitionKind::IDK_VECTOR) {
					continue;
				}
				const bool hasNodeMapping = metadata_.findIndexRootOptional(snapshot, definition.index_id).has_value();
				const bool hasManifest =
						metadata_.findVectorManifestOptional(snapshot, definition.index_id).has_value();
				if (hasNodeMapping != hasManifest) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph Vector Index has an incomplete mapping/Manifest publication");
				}
			}
		}
		std::map<uint64_t, std::vector<NativeVectorMutation>> vectorMutations;
		const auto stageVectorSemanticDelta = [&vector, &snapshot, &vectorMutations,
											   this](uint64_t nodeId, std::span<const uint64_t> oldLabels,
													 const PropertyCollectionRoot *oldProperties,
													 std::span<const uint64_t> newLabels,
													 const PropertyCollectionRoot *newProperties) {
			if (!vector.has_value()) {
				return;
			}
			for (NativeVectorMutation mutation:
				 vector->planNode(snapshot, nodeId, oldLabels, oldProperties, newLabels, newProperties)) {
				auto &perIndex = vectorMutations[mutation.index_id];
				if (perIndex.size() == options_.maximum_vector_records) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph Vector mutations exceed the configured per-Index batch bound");
				}
				perIndex.push_back(std::move(mutation));
			}
		};
		PageBackedConstraintMutationAdapter constraintAdapter(
				snapshot, graph_, indexes_, metadata_,
				[&resolvePropertyName](const MetadataSnapshotPin &, uint32_t token) {
					const std::optional<std::string> name = resolvePropertyName(token);
					return name.value_or(std::string{});
				},
				[&resolveSchemaToken](const MetadataSnapshotPin &, std::string_view name) {
					const std::optional<uint64_t> token =
							resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
					return token.value_or(0U);
				},
				[&resolveSchemaToken](const MetadataSnapshotPin &, std::string_view name) {
					const std::optional<uint64_t> token =
							resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
					return token.value_or(0U);
				});
		ConstraintMutationValidator validator(
				constraintAdapter.makeAdapter(),
				{options_.maximum_constraint_entity_scan, options_.maximum_constraint_support_collisions});
		const std::vector<ConstraintSupportTreeMutationPlan> supportPlans =
				validator.validate(constraintDefinitions, constraintBatch);

		bool hasStagedWork = false;
		LogicalGraphMutationBatchResult result;
		result.created_node_ids.reserve(nodes.size());
		for (size_t ordinal = 0U; ordinal < nodes.size(); ++ordinal) {
			const uint64_t nodeId =
					batch.creation_identity_plan.has_value()
							? transaction.createNodeWithReservedId(batch.creation_identity_plan->node_ids.at(ordinal),
																   nodes[ordinal].labels)
							: transaction.createNode(nodes[ordinal].labels);
			result.created_node_ids.push_back(nodeId);
			const PropertyCollectionRoot createdRoot = nodeRoot(nodeId, nodes[ordinal].properties.entries);
			stageVectorSemanticDelta(nodeId, {}, nullptr, nodes[ordinal].labels,
									 nodes[ordinal].properties.entries.empty() ? nullptr : &createdRoot);
			scalar.stageNodeLabels(transaction, nodeId, nodes[ordinal].labels, false);
			if (!nodes[ordinal].properties.entries.empty()) {
				PropertyCollectionRoot root = createdRoot;
				transaction.replaceNodeProperty(root);
				scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, nodeId, nodes[ordinal].labels,
									   root, false);
			}
			hasStagedWork = true;
		}
		result.created_edge_ids.reserve(batch.edge_creates.size());
		for (size_t ordinal = 0U; ordinal < batch.edge_creates.size(); ++ordinal) {
			const LogicalGraphBatchEdgeCreateRequest &request = batch.edge_creates[ordinal];
			const uint64_t sourceNodeId = resolveEntityId(request.source, result);
			const uint64_t targetNodeId = resolveEntityId(request.target, result);
			uint64_t typeToken = RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN;
			if (!request.type.empty()) {
				const std::optional<uint64_t> resolved =
						resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, request.type);
				if (!resolved.has_value() || *resolved == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation could not resolve a relationship-type Token in its snapshot");
				}
				typeToken = *resolved;
			}
			ResolvedProperties properties = resolveProperties(request.properties, resolvePropertyKey);
			if (const auto override = createdEdgeProperties.find(ordinal); override != createdEdgeProperties.end()) {
				properties = override->second;
			}
			const uint64_t edgeId =
					batch.creation_identity_plan.has_value()
							? transaction.createEdgeWithReservedId(batch.creation_identity_plan->edge_ids.at(ordinal),
															   sourceNodeId, targetNodeId,
															   typeToken)
							: transaction.createEdge(sourceNodeId, targetNodeId, typeToken);
			result.created_edge_ids.push_back(edgeId);
			scalar.stageEdgeType(transaction, edgeId, typeToken, false);
			if (!properties.entries.empty()) {
				PropertyCollectionRoot root = edgeRoot(edgeId, properties.entries);
				transaction.replaceEdgeProperty(root);
				scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {}, root, false);
			}
			hasStagedWork = true;
		}
		for (const auto &[nodeId, mutation]: persistedNodes) {
			const PageBackedGraphNode old = graph_.readNode(snapshot, nodeId);
			const std::vector<uint64_t> newLabels = mutation.labels.value_or(old.label_token_ids);
			const std::vector<PropertyCollectionRootEntry> newProperties =
					mutation.properties.has_value() ? mutation.properties->entries : old.properties;
			if (newLabels == old.label_token_ids && newProperties == old.properties) {
				continue;
			}
			const PropertyCollectionRoot oldRoot = nodeRoot(nodeId, old.properties);
			const PropertyCollectionRoot newRoot = nodeRoot(nodeId, newProperties);
			stageVectorSemanticDelta(nodeId, old.label_token_ids, &oldRoot, newLabels, &newRoot);
			scalar.stageNodeLabels(transaction, nodeId, old.label_token_ids, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, nodeId, old.label_token_ids,
								   oldRoot, true);
			scalar.stageNodeLabels(transaction, nodeId, newLabels, false);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, nodeId, newLabels, newRoot,
								   false);
			if (newLabels != old.label_token_ids) {
				transaction.replaceNodeLabels(nodeId, newLabels);
			}
			if (newProperties != old.properties) {
				transaction.replaceNodeProperty(newRoot);
			}
			hasStagedWork = true;
		}
		for (const auto &[edgeId, properties]: persistedEdges) {
			const PageBackedGraphEdge old = graph_.readEdge(snapshot, edgeId);
			if (properties.entries == old.properties) {
				continue;
			}
			const PropertyCollectionRoot oldRoot = edgeRoot(edgeId, old.properties);
			const PropertyCollectionRoot newRoot = edgeRoot(edgeId, properties.entries);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {}, oldRoot, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {}, newRoot, false);
			transaction.replaceEdgeProperty(newRoot);
			hasStagedWork = true;
		}

		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			if (request.detach || !transaction.nodeHasIncidentEdges(request.node_id)) {
				continue;
			}
			if (deletedEdges.size() == std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native logical graph Node deletion overflowed its incident-Edge validation bound");
			}
			// One extra record is sufficient to prove that the caller omitted an
			// incident Edge while preserving a finite validation scan.
			const std::vector<uint64_t> incident =
					transaction.collectIncidentEdgeIds(request.node_id, deletedEdges.size() + 1U);
			if (std::ranges::any_of(incident,
									[&deletedEdges](uint64_t edgeId) { return !deletedEdges.contains(edgeId); })) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native logical graph Node deletion has an incident Edge outside its batch delete set");
			}
		}
		std::vector<uint64_t> orderedErasedEdges(deletedEdges.begin(), deletedEdges.end());
		std::ranges::sort(orderedErasedEdges);
		for (const uint64_t edgeId: orderedErasedEdges) {
			const PageBackedGraphEdge old = graph_.readEdge(snapshot, edgeId);
			scalar.stageEdgeType(transaction, edgeId, old.type_id, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {},
								   edgeRoot(edgeId, old.properties), true);
			hasStagedWork = true;
		}
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			const PageBackedGraphNode old = graph_.readNode(snapshot, request.node_id);
			const PropertyCollectionRoot oldRoot = nodeRoot(request.node_id, old.properties);
			stageVectorSemanticDelta(request.node_id, old.label_token_ids, &oldRoot, {}, nullptr);
			scalar.stageNodeLabels(transaction, request.node_id, old.label_token_ids, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, request.node_id,
								   old.label_token_ids, oldRoot, true);
			transaction.eraseNode(request.node_id);
			hasStagedWork = true;
		}

		if (catalogSession.has_value() &&
			(!nodes.empty() || !batch.edge_creates.empty() || !orderedErasedEdges.empty() || !batch.node_deletes.empty())) {
			const std::optional<NativeGraphCardinalityStatistics> current =
					system_catalog_->readGraphCardinalityStatistics(snapshot);
			// Older files that already have a Catalog root but no complete record stay
			// estimate-free. Backfilling them requires a separately bounded maintenance
			// operation; silently seeding from an ID high-water mark would be wrong.
			if (current.has_value() ||
				!metadata_.findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value()) {
				const NativeGraphCardinalityStatistics base = current.value_or(NativeGraphCardinalityStatistics{});
				catalogSession->setGraphCardinalityStatistics(
						{checkedCardinalityDelta(base.node_count, nodes.size(), batch.node_deletes.size(),
													"native graph Node cardinality counter overflowed or underflowed"),
						 checkedCardinalityDelta(base.edge_count, batch.edge_creates.size(), orderedErasedEdges.size(),
													"native graph Edge cardinality counter overflowed or underflowed")});
			}
		}

		for (const ConstraintSupportTreeMutationPlan &plan: supportPlans) {
			const uint64_t entityId = resolveSupportEntityId(plan, result);
			if (plan.erased_canonical_key.has_value()) {
				scalar.stageConstraintSupportErase(transaction, plan.constraint_id, *plan.erased_canonical_key,
												   entityId);
			}
			if (plan.upserted_canonical_key.has_value()) {
				scalar.stageConstraintSupportUpsert(transaction, plan.constraint_id, *plan.upserted_canonical_key,
													entityId);
			}
		}
		for (auto &[indexId, mutations]: vectorMutations) {
			const auto definition = std::ranges::find(indexDefinitions, indexId, &IndexDefinition::index_id);
			if (definition == indexDefinitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native logical graph Vector planner returned an unknown Vector Index identity");
			}
			std::ranges::sort(mutations, {}, [](const NativeVectorMutation &mutation) {
				return std::tuple<uint64_t, uint8_t>{mutation.node_id, static_cast<uint8_t>(mutation.kind)};
			});
			const bool hasNodeMapping = metadata_.findIndexRootOptional(snapshot, indexId).has_value();
			const bool hasManifest = metadata_.findVectorManifestOptional(snapshot, indexId).has_value();
			if (hasNodeMapping != hasManifest) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native logical graph Vector Index has an incomplete mapping/Manifest publication");
			}
			if (hasNodeMapping) {
				NativeGraphVectorDeltaOptions delta;
				delta.first_slot = PhysicalSlotId(1U);
				delta.last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
				delta.maximum_records = options_.maximum_vector_records;
				delta.maximum_raw_pages = options_.maximum_vector_pages;
				delta.rebuild_threshold = options_.vector_delta_rebuild_threshold;
				delta.maximum_overlay_entries = options_.maximum_vector_delta_overlay_entries;
				delta.maximum_delta_pages = options_.maximum_vector_delta_pages;
				transaction.stageVectorIndexDelta(indexes_, vector_pages_, vector_manifests_, *definition, mutations,
												  delta);
				continue;
			}

			std::vector<NativeGraphVectorBootstrapRecord> records;
			records.reserve(mutations.size());
			for (const NativeVectorMutation &mutation: mutations) {
				if (mutation.kind != NativeVectorMutationKind::NVMK_INSERT) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph Vector bootstrap found an erase without published Vector state");
				}
				records.push_back({mutation.node_id, mutation.encoded_bfloat16});
			}
			if (records.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native logical graph Vector bootstrap requires a complete nonempty first population");
			}
			NativeGraphVectorBootstrapOptions bootstrap;
			bootstrap.first_slot = PhysicalSlotId(1U);
			bootstrap.last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			bootstrap.maximum_vectors = options_.maximum_vector_records;
			bootstrap.maximum_pages = options_.maximum_vector_pages;
			transaction.stageVectorIndexBootstrap(indexes_, vector_pages_, vector_manifests_, *definition, records,
												  bootstrap);
		}
		if (catalogSession.has_value() && catalogSession->changed()) {
			catalogSession->finish();
			hasStagedWork = true;
		}
		if (!hasStagedWork) {
			if (batch.creation_identity_plan.has_value() && (!batch.creation_identity_plan->node_id_ranges.empty() ||
															 !batch.creation_identity_plan->edge_id_ranges.empty())) {
				return {std::move(result), transaction.commit()};
			}
			const Superblock base = transaction.baseSuperblock();
			transaction.rollback();
			return {std::move(result), {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, base, {}}};
		}
		const NativeGraphWriteCommitResult commit = transaction.commit();
		if (!commit.committed()) {
			result = {};
		}
		return {std::move(result), commit};
	}

} // namespace graph::storage
