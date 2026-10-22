/**
 * @file NativeGraphMutationExecutor.cpp
 * @brief Declarative logical-batch execution over native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"
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

        class NativeMutationCheckpoint final : public CancellationCheckpoint {
        public:
            explicit NativeMutationCheckpoint(const std::function<void()> &callback) : callback_(callback) {}
            void check() const override { if (callback_) callback_(); }
        private:
            const std::function<void()> &callback_;
        };

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
						  const std::function<std::optional<uint32_t>(std::string_view)> &resolvePropertyKey,
						  const std::function<void()> &checkpoint) {
			if (checkpoint) checkpoint();
			// Borrow only ordering descriptors; resolved entries own the sole new
			// copy of each input value and outlive these synchronous references.
			std::vector<const LogicalGraphWriteProperty *> ordered;
			ordered.reserve(properties.size());
			for (const auto &property : properties) {
				if (checkpoint) checkpoint();
				ordered.push_back(&property);
			}
			std::ranges::sort(ordered, [&](const auto *left, const auto *right) {
				if (checkpoint) checkpoint();
				return left->key < right->key;
			});
			for (size_t index = 0U; index < ordered.size(); ++index) {
				if (checkpoint) checkpoint();
				if (ordered[index]->key.empty() || (index != 0U && ordered[index - 1U]->key == ordered[index]->key)) {
					throw std::invalid_argument(
							"native logical graph mutation requires nonempty, unique Property keys per entity");
				}
			}
			ResolvedProperties result;
			result.entries.reserve(ordered.size());
			for (const auto *descriptor: ordered) {
				if (checkpoint) checkpoint();
				const LogicalGraphWriteProperty &property = *descriptor;
				const std::optional<uint32_t> token = resolvePropertyKey(property.key);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native logical graph mutation could not resolve a Property-key Token in its snapshot");
				}
				if (checkpoint) checkpoint();
				result.entries.push_back({*token, property.value});
			}
			std::ranges::sort(result.entries, [&](const auto &left, const auto &right) {
				if (checkpoint) checkpoint();
				return left.key_token < right.key_token;
			});
			for (size_t index = 1U; index < result.entries.size(); ++index) {
				if (checkpoint) checkpoint();
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
				const std::function<std::optional<uint32_t>(std::string_view)> &resolvePropertyKey,
				const std::function<void()> &checkpoint) {
			if (checkpoint) checkpoint();
			std::unordered_set<std::string_view> names;
			std::unordered_set<uint32_t> tokens;
			names.reserve(properties.size());
			tokens.reserve(properties.size());
			for (const LogicalGraphWriteProperty &property: properties) {
				if (checkpoint) checkpoint();
				if (property.key.empty() || !names.emplace(property.key).second) {
					throw std::invalid_argument(
							"native logical graph mutation requires nonempty, unique Property keys per entity");
				}
				const std::optional<uint32_t> token = resolvePropertyKey(property.key);
				if (!token.has_value() || *token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native logical graph mutation could not resolve a Property-key Token in its snapshot");
				}
				if (checkpoint) checkpoint();
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
        const bool covered = options_.covered_adjacency_candidate_index != 0U;
        if ((covered && ((options_.covered_adjacency_candidate_index >> 62U) ||
                !options_.maximum_covered_adjacency_changes || options_.maximum_covered_adjacency_changes > 64U ||
                !options_.covered_adjacency_workspace_bytes || options_.covered_adjacency_workspace_bytes > 1024U * 1024U)) ||
            (!covered && (options_.maximum_covered_adjacency_changes || options_.covered_adjacency_workspace_bytes)))
            throw std::invalid_argument("covered adjacency candidate requires an explicit namespace and bounded workspace");
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
									 const NativeGraphMutationFenceValidator &expectedFence, const LogicalGraphMutationReadControl &readControl) const {
		return executeImpl(batch, {}, 0U, expectedFence, readControl);
	}

	NativeGraphMutationExecutionResult NativeGraphMutationExecutor::executeWithPropertyPatches(
			const LogicalGraphMutationBatch &batch, std::span<const LogicalGraphPropertyPatchRequest> patches,
			size_t maximumPropertyEntries, const NativeGraphMutationFenceValidator &expectedFence,
			const LogicalGraphMutationReadControl &readControl) const {
		if (!expectedFence || maximumPropertyEntries == 0U)
			throw std::invalid_argument("native Property patches require a fence and positive entry bound");
		return executeImpl(batch, patches, maximumPropertyEntries, expectedFence, readControl);
	}

	NativeGraphMutationExecutionResult NativeGraphMutationExecutor::executeImpl(
			const LogicalGraphMutationBatch &batch, std::span<const LogicalGraphPropertyPatchRequest> patches,
			size_t maximumPropertyEntries, const NativeGraphMutationFenceValidator &expectedFence,
			const LogicalGraphMutationReadControl &readControl) const {
		debug::ScopedPerfTimer timer("native_graph.execute");
		// Fold complete old/new memberships before updating estimates. In particular,
		// removing and re-adding the same label must not underflow or double count.
		using TokenCountKey = std::pair<SystemCatalogSchemaTokenKind, uint64_t>;
		struct TokenCountDelta { uint64_t added = 0U; uint64_t removed = 0U; };
		std::map<TokenCountKey, TokenCountDelta> tokenCountDeltas;
		bool collectLabelCounts = false;
		bool collectTypeCounts = false;
		const auto accountToken = [&](SystemCatalogSchemaTokenKind kind, uint64_t token, bool erase) {
			if (!(kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL ? collectLabelCounts : collectTypeCounts)) return;
			auto &delta = tokenCountDeltas[{kind, token}];
			auto &count = erase ? delta.removed : delta.added;
			if (count == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "token cardinality delta overflowed");
			}
			++count;
		};
		const auto accountLabels = [&](std::span<const uint64_t> labels, bool erase) {
			for (uint64_t token : labels) accountToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, token, erase);
		};
		if (readControl.checkpoint) readControl.checkpoint();
		validateShape(batch);
		size_t patchEntries = 0U;
		const auto chargePatchEntries = [&](size_t count) {
			if (count > maximumPropertyEntries - patchEntries)
				throw std::runtime_error("native Property patches exceeded their entry bound");
			patchEntries += count;
		};
		if (patches.size() > maximumPropertyEntries)
			throw std::runtime_error("native Property patches exceeded their target bound");
		std::set<std::tuple<uint8_t, uint8_t, uint64_t>> patchTargets;
		for (const auto &replacement : batch.property_replacements)
			patchTargets.insert(referenceKey(replacement.entity));
		for (const auto &patch : patches) {
			if (readControl.checkpoint) readControl.checkpoint();
			validateReference(batch, patch.entity, "native Property patch");
			if (!patchTargets.insert(referenceKey(patch.entity)).second)
				throw std::invalid_argument("native Property patches overlap a replacement or another patch");
			if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED &&
				((patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
				  std::ranges::any_of(batch.node_deletes, [&](const auto &v) { return v.node_id == patch.entity.value; })) ||
				 (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
				  std::ranges::any_of(batch.edge_deletes, [&](const auto &v) { return v.edge_id == patch.entity.value; }))))
				throw std::invalid_argument("native Property patches cannot target a deleted entity");
			chargePatchEntries(patch.sets.size());
			chargePatchEntries(patch.removals.size());
			if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
				chargePatchEntries(batch.node_creates.at(static_cast<size_t>(patch.entity.value)).properties.size());
			else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE)
				chargePatchEntries(batch.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties.size());
			std::set<std::string_view> names;
			for (const auto &property : patch.sets) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (property.key.empty() || property.value.getType() == PropertyType::NULL_TYPE ||
					!names.insert(property.key).second)
					throw std::invalid_argument("native Property patch sets require unique nonempty keys and non-null values");
			}
			for (const auto &name : patch.removals) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (name.empty() || !names.insert(name).second)
					throw std::invalid_argument("native Property patch removals must be unique and disjoint from sets");
			}
		}
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
        const auto finalizeCoveredAdjacency = [&] {
            if (!options_.covered_adjacency_candidate_index) return;
            NativeMutationCheckpoint checkpoint(readControl.checkpoint);
            auto memory = std::make_shared<query::execution::MaterializedByteBudget>(options_.covered_adjacency_workspace_bytes);
            // Scalar indexes, constraints and counts have already been staged by
            // this coordinator. Do not stage a second copy of their deltas.
            transaction.stageCoveredAdjacencyCandidate(options_.covered_adjacency_candidate_index,
                options_.maximum_covered_adjacency_changes, std::move(memory), &checkpoint);
        };
		NativeReadSession oldStateReads(snapshot);
        const auto coordinateNativeVectors = [&] {
            NativeGraphVectorDeltaOptions vectors;
            vectors.maximum_records = std::min(size_t(128), options_.maximum_vector_records);
            vectors.maximum_raw_pages = std::min(size_t(128), options_.maximum_vector_pages);
            vectors.maximum_overlay_entries = std::min(size_t(4096), options_.maximum_vector_delta_overlay_entries);
            vectors.rebuild_threshold = std::min(vectors.maximum_overlay_entries, options_.vector_delta_rebuild_threshold);
            vectors.maximum_delta_pages = std::min(size_t(128), options_.maximum_vector_delta_pages);
            transaction.coordinateHybridPropertyVectors(vector_pages_, vector_manifests_, vectors);
        };
        struct NativePropertyTokens {
            utils::OwnedHeapReservation credit;
            NativeGraphWritePlanner &writer;
            SystemCatalog *catalog;
            const NativeGraphMutationTokenResolvers &callbacks;
            const MetadataSnapshotPin &source;
            std::optional<NativeGraphWriteTransaction> transaction;
            std::optional<SystemCatalogWriteSession> session;
            std::map<std::string, uint32_t, std::less<>> names;
            NativePropertyTokens(std::shared_ptr<std::pmr::memory_resource> memory, NativeGraphWritePlanner &w,
                SystemCatalog *c, const NativeGraphMutationTokenResolvers &resolvers, const MetadataSnapshotPin &snapshot)
                : credit(std::move(memory)), writer(w), catalog(c), callbacks(resolvers), source(snapshot) {}
            std::optional<uint32_t> existing(std::string_view name) const {
                return catalog ? catalog->findPropertyToken(source, name) : callbacks.property_key_token(source, name);
            }
            std::optional<uint32_t> ensure(std::string_view name) {
                if (const auto token = existing(name)) return token;
                if (!catalog) fail(FormatError::FERR_UNSUPPORTED_VERSION, "new native property names require the Catalog coordinator");
                if (const auto found = names.find(name); found != names.end()) return found->second;
                if (names.size() == 64U || name.size() > (1024U * 1024U - 65536U) / 4U)
                    fail(FormatError::FERR_INVALID_STATE, "native property dictionary input exceeds its bounded frontier");
                const size_t extra = 4U * (name.size() + 128U);
                credit.set((credit.bytes() ? credit.bytes() : 65536U) + extra);
                if (!transaction) {
                    transaction.emplace(writer.beginTransaction());
                    if (transaction->baseSuperblock() != source.superblock())
                        fail(FormatError::FERR_INVALID_STATE, "native dictionary source changed while staging fields");
                    session.emplace(catalog->beginWrite(*transaction));
                }
                const auto token = session->ensurePropertyToken(name); names.emplace(name, token); return token;
            }
        };
        const auto finishNativeTokens = [&](NativePropertyTokens &tokens) {
            if (!tokens.session) return;
            tokens.session->finish(); tokens.session.reset();
            std::map<std::string, uint32_t, std::less<>>().swap(tokens.names);
            transaction.adoptHybridPropertyCatalog(std::move(*tokens.transaction), std::move(tokens.credit));
            tokens.transaction.reset();
        };
        // Property-only native replacements use the field lifecycle writer,
        // rather than constructing competing ROW roots inside a native group.
        // Mixed ROW/native work uses the ordinary logical coordinator below.
        if (!options_.covered_adjacency_candidate_index && snapshot.superblock().hybrid_property_descriptor_root.isValid() && patches.empty() &&
            !batch.property_replacements.empty() && batch.node_creates.empty() && batch.edge_creates.empty() &&
            batch.node_label_replacements.empty() && batch.node_deletes.empty() && batch.edge_deletes.empty() &&
            !batch.creation_identity_plan) {
            NativeMutationCheckpoint checkpoint(readControl.checkpoint);
            const auto ownerOf = [](const LogicalGraphPropertyMutationRequest &request) {
                return EntityPropertyKey{request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE ?
                    EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, request.entity.value};
            };
            bool native = false, allNative = true;
            for (const auto &request : batch.property_replacements) {
                checkpoint.check();
                const bool routed = transaction.isHybridPropertyOwner(ownerOf(request), &checkpoint);
                native |= routed; allNative &= routed;
            }
            if (native && allNative) {
                if (batch.property_replacements.size() > 64U)
                    fail(FormatError::FERR_INVALID_STATE, "native public property batch exceeds64owners");
                auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U);
                utils::OwnedHeapReservation inputCredit(memory);
                size_t bytes = batch.property_replacements.size() * sizeof(PropertyCollectionRoot), entries = 0;
                for (const auto &request : batch.property_replacements) {
                    if (request.properties.size() > 4096U - entries)
                        fail(FormatError::FERR_INVALID_STATE, "native public property batch exceeds4096fields");
                    entries += request.properties.size();
                    for (const auto &property : request.properties) {
                        checkpoint.check();
                        const auto size = CanonicalPropertyValueCodec::encodedSize(property.value);
                        if (size > 1024U * 1024U - bytes || sizeof(PropertyCollectionRootEntry) > 1024U * 1024U - bytes - size)
                            fail(FormatError::FERR_INVALID_STATE, "native public property input exceeds one MiB");
                        bytes += size + sizeof(PropertyCollectionRootEntry);
                    }
                }
                inputCredit.set(bytes);
                utils::OwnedAllocationScope allocation(memory);
                NativePropertyTokens nativeTokens(memory, writer_, system_catalog_, tokens_, snapshot);
                std::vector<PropertyCollectionRoot> roots; roots.reserve(batch.property_replacements.size());
                const auto resolveExisting = [&](std::string_view key) { return nativeTokens.ensure(key); };
                for (const auto &request : batch.property_replacements) {
                    auto resolved = resolveProperties(request.properties, resolveExisting, readControl.checkpoint);
                    roots.push_back(request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE ?
                        nodeRoot(request.entity.value, std::move(resolved.entries)) : edgeRoot(request.entity.value, std::move(resolved.entries)));
                }
                transaction.stageHybridPropertyMaps(roots, NativeHybridPropertyMapMode::NHPMM_REPLACE, memory, &checkpoint);
                std::vector<PropertyCollectionRoot>().swap(roots); inputCredit.set(0);
                coordinateNativeVectors(); finishNativeTokens(nativeTokens);
                checkpoint.check();
                return {{}, transaction.commit()};
            }
        }
        // A selected public patch keeps unchanged native fields borrowed and
        // stages only sets/removals, preserving Cypher removal semantics.
        if (!options_.covered_adjacency_candidate_index && snapshot.superblock().hybrid_property_descriptor_root.isValid() && !patches.empty() &&
            batch.property_replacements.empty() && batch.node_creates.empty() && batch.edge_creates.empty() &&
            batch.node_label_replacements.empty() && batch.node_deletes.empty() && batch.edge_deletes.empty() &&
            !batch.creation_identity_plan) {
            NativeMutationCheckpoint checkpoint(readControl.checkpoint);
            const auto ownerOf = [](const LogicalGraphPropertyPatchRequest &patch) {
                return EntityPropertyKey{patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE ?
                    EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, patch.entity.value};
            };
            bool native = false, allNative = true;
            for (const auto &patch : patches) {
                checkpoint.check(); const bool routed = transaction.isHybridPropertyOwner(ownerOf(patch), &checkpoint);
                native |= routed; allNative &= routed;
            }
            if (native && allNative) {
                if (patches.size() > 64U) fail(FormatError::FERR_INVALID_STATE, "native public patches exceed64owners");
                auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U);
                utils::OwnedAllocationScope allocation(memory);
                NativePropertyTokens nativeTokens(memory, writer_, system_catalog_, tokens_, snapshot);
                {
                    utils::OwnedVector<NativeHybridPropertyColumnReplacement> cells{
                        utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(memory)};
                    const auto resolve = [&](std::string_view name) { return nativeTokens.ensure(name); };
                    for (const auto &patch : patches) {
                        const auto owner = ownerOf(patch);
                        for (const auto &property : patch.sets) {
                            checkpoint.check(); const auto token = resolve(property.key);
                            if (!token || !*token) fail(FormatError::FERR_UNSUPPORTED_VERSION, "native public patches require existing property tokens");
                            if (cells.size() == 64U) fail(FormatError::FERR_INVALID_STATE, "native public patches exceed64field operations");
                            cells.push_back({owner, *token, OwnedPropertyValue(property.value), false});
                        }
                        for (const auto &name : patch.removals) {
                            checkpoint.check(); const auto token = nativeTokens.existing(name);
                            if (!token) continue; // Removing an unknown field is a logical no-op.
                            if (cells.size() == 64U) fail(FormatError::FERR_INVALID_STATE, "native public patches exceed64field operations");
                            cells.push_back({owner, *token, {}, true});
                        }
                    }
                    if (cells.empty()) {
                        const auto base = transaction.baseSuperblock(); transaction.rollback();
                        return {{}, {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, base, {}}};
                    }
                    transaction.stageHybridPropertyColumns(cells, memory, &checkpoint, true);
                }
                coordinateNativeVectors(); finishNativeTokens(nativeTokens); checkpoint.check(); return {{}, transaction.commit()};
            }
        }
        // The native lifecycle coordinator owns the complete Node/Edge deletion
        // closure. Detach discovery is bounded and revalidated under the writer.
        if (!options_.covered_adjacency_candidate_index && snapshot.superblock().hybrid_property_descriptor_root.isValid() && patches.empty() &&
            batch.property_replacements.empty() && batch.node_creates.empty() && batch.edge_creates.empty() &&
            batch.node_label_replacements.empty() && !batch.creation_identity_plan &&
            (!batch.node_deletes.empty() || !batch.edge_deletes.empty())) {
            NativeMutationCheckpoint checkpoint(readControl.checkpoint); bool native = false, allNative = true;
            for (const auto &node : batch.node_deletes) {
                checkpoint.check(); const bool routed = transaction.isHybridPropertyOwner({EntityPropertyOwnerKind::EPOK_NODE, node.node_id}, &checkpoint);
                native |= routed; allNative &= routed;
            }
            for (const auto &edge : batch.edge_deletes) {
                checkpoint.check(); const bool routed = transaction.isHybridPropertyOwner({EntityPropertyOwnerKind::EPOK_EDGE, edge.edge_id}, &checkpoint);
                native |= routed; allNative &= routed;
            }
            if (native && allNative) {
                if (batch.node_deletes.size() > 64U || batch.edge_deletes.size() > 64U - batch.node_deletes.size())
                    fail(FormatError::FERR_INVALID_STATE, "native public deletion exceeds64owners");
                auto memory = std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U);
                utils::OwnedAllocationScope allocation(memory);
                std::array<EntityPropertyKey, 64> owners{}; size_t count = 0;
                const auto append = [&](EntityPropertyKey owner) {
                    const auto selected = std::span(owners).first(count);
                    if (std::ranges::find(selected, owner) != selected.end()) return;
                    if (count == owners.size()) fail(FormatError::FERR_INVALID_STATE, "native detach closure exceeds64owners");
                    allNative &= transaction.isHybridPropertyOwner(owner, &checkpoint);
                    owners[count++] = owner;
                };
                for (const auto &node : batch.node_deletes) { checkpoint.check(); append({EntityPropertyOwnerKind::EPOK_NODE, node.node_id}); }
                for (const auto &edge : batch.edge_deletes) { checkpoint.check(); append({EntityPropertyOwnerKind::EPOK_EDGE, edge.edge_id}); }
                for (const auto &node : batch.node_deletes) if (node.detach) {
                    checkpoint.check(); const auto limit = std::min(size_t(64), node.maximum_detach_edges);
                    // Standard set/vector and fixed Entity-page scratch used by
                    // the bounded canonical adjacency discovery are paid first.
                    utils::OwnedHeapReservation discovery(memory, 32U * 1024U + 256U * limit);
                    const auto incident = transaction.collectIncidentEdgeIds(node.node_id, limit);
                    for (const auto edge : incident) { checkpoint.check(); append({EntityPropertyOwnerKind::EPOK_EDGE, edge}); }
                }
                if (allNative) {
                    transaction.stageHybridPropertyOwnerDeletion(std::span(owners).first(count), memory, &checkpoint);
                    coordinateNativeVectors(); checkpoint.check(); return {{}, transaction.commit()};
                }
            }
        }

		std::optional<SystemCatalogWriteSession> catalogSession;
		if (system_catalog_ != nullptr) {
			catalogSession.emplace(system_catalog_->beginWrite(transaction));
			collectLabelCounts = system_catalog_->hasCompleteTokenCardinality(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL);
			collectTypeCounts = system_catalog_->hasCompleteTokenCardinality(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE);
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

		const utils::OwnedStdVector<ConstraintDefinition> constraintDefinitions = constraint_definitions_(snapshot);
		if (constraintDefinitions.size() > options_.maximum_constraint_definitions)
			fail(FormatError::FERR_INVALID_STATE, "native logical graph mutation constraint reader exceeded its bound");
		// The validator returns immediately without definitions. Avoid a second
		// owning copy of every creation/property payload in that common case.
		LogicalGraphMutationBatch constraintBatch = constraintDefinitions.empty() ? LogicalGraphMutationBatch{} : batch;
		std::vector<ResolvedNodeCreate> nodes;
		nodes.reserve(batch.node_creates.size());
		for (const LogicalGraphNodeCreateRequest &request: batch.node_creates) {
			nodes.push_back({resolveLabels(request.labels,
											   [&resolveSchemaToken](std::string_view name) {
												   return resolveSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL,
																		 name);
											   }),
							 resolveProperties(request.properties, resolvePropertyKey, readControl.checkpoint)});
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
			validatePropertyKeysAndTokens(request.properties, resolvePropertyKey, readControl.checkpoint);
		}
		// Read old states in logical-ID order so snapshot cursors can share page
		// and directory descents across the transaction, independent of request order.
		std::map<uint64_t, PersistedNodeMutation> persistedNodes;
		std::map<uint64_t, ResolvedProperties> persistedEdges;
		// Do not materialize a second full resolved-edge batch. Bulk edge ingest
		// already owns the logical request list; only statement-local property
		// replacements need an additional sparse override map.
		std::unordered_map<uint64_t, ResolvedProperties> createdEdgeProperties;
		for (const LogicalGraphPropertyMutationRequest &request: batch.property_replacements) {
			ResolvedProperties resolved = resolveProperties(request.properties, resolvePropertyKey, readControl.checkpoint);
			if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
				if (request.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
					persistedNodes[request.entity.value].properties = std::move(resolved);
				} else {
					persistedEdges.emplace(request.entity.value, std::move(resolved));
				}
			} else if (request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				nodes.at(static_cast<size_t>(request.entity.value)).properties = std::move(resolved);
			} else {
				createdEdgeProperties.insert_or_assign(request.entity.value, std::move(resolved));
			}
		}
		std::vector<const LogicalGraphPropertyPatchRequest *> orderedPatches;
		std::optional<PageBackedConstraintPropertySelection> constraintSelection;
		orderedPatches.reserve(patches.size());
		for (const auto &patch : patches) {
			if (readControl.checkpoint) readControl.checkpoint();
			orderedPatches.push_back(&patch);
		}
		std::ranges::sort(orderedPatches, [&](const auto *left, const auto *right) {
			if (readControl.checkpoint) readControl.checkpoint();
			return referenceKey(left->entity) < referenceKey(right->entity);
		});
		const SelectedPropertyReadAdmission sourceAdmission = makeSelectedPropertyBatchAdmission(
			[&](size_t bytes) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (readControl.property_admission) readControl.property_admission(bytes);
			});
		for (const auto *descriptor : orderedPatches) {
			if (readControl.checkpoint) readControl.checkpoint();
			const auto &patch = *descriptor;
			ResolvedProperties source;
			if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
				source = std::move(nodes.at(static_cast<size_t>(patch.entity.value)).properties);
			} else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
				source = resolveProperties(batch.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties,
					resolvePropertyKey, readControl.checkpoint);
			} else {
				const std::array<uint64_t, 1U> ids{patch.entity.value};
				if (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
					static_cast<void>(graph_.readNodeRecord(snapshot, ids.front(), &oldStateReads));
					source.entries = std::move(graph_.readNodePropertiesSorted(snapshot, ids, &oldStateReads,
						&sourceAdmission).front());
				} else {
					static_cast<void>(graph_.readEdgeTopology(snapshot, ids.front(), &oldStateReads));
					source.entries = graph_.readEdgeProperties(snapshot, ids.front(), &oldStateReads, &sourceAdmission);
				}
			}
			if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED)
				chargePatchEntries(source.entries.size());
			ResolvedProperties sets = resolveProperties(patch.sets, resolvePropertyKey, readControl.checkpoint);
			std::set<uint32_t> operationTokens;
			for (const auto &entry : sets.entries) operationTokens.insert(entry.key_token);
			std::set<uint32_t> removals;
			for (const auto &name : patch.removals) {
				if (readControl.checkpoint) readControl.checkpoint();
				std::optional<uint32_t> token;
				if (const auto staged = stagedPropertyNames.find(name); staged != stagedPropertyNames.end())
					token = staged->second;
				else if (system_catalog_ != nullptr)
					token = system_catalog_->findPropertyToken(snapshot, name);
				else
					token = tokens_.property_key_token(snapshot, name);
				if (!token) continue; // Removing an unknown name must not create a token.
				if (*token == 0U || !operationTokens.insert(*token).second)
					fail(FormatError::FERR_INVALID_STATE, "native Property patch resolved distinct keys to one Token");
				removals.insert(*token);
			}
			ResolvedProperties final;
			size_t finalSize = source.entries.size();
			const auto sourceContains = [&](uint32_t token) {
				const auto found = std::lower_bound(source.entries.begin(), source.entries.end(), token,
					[](const auto &entry, uint32_t key) { return entry.key_token < key; });
				return found != source.entries.end() && found->key_token == token;
			};
			for (uint32_t token : removals) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (sourceContains(token)) --finalSize;
			}
			for (const auto &entry : sets.entries) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (!sourceContains(entry.key_token)) ++finalSize;
			}
			final.entries.reserve(finalSize);
			size_t oldIndex = 0U, setIndex = 0U;
			while (oldIndex < source.entries.size() || setIndex < sets.entries.size()) {
				if (readControl.checkpoint) readControl.checkpoint();
				if (setIndex < sets.entries.size() && (oldIndex == source.entries.size() ||
					sets.entries[setIndex].key_token <= source.entries[oldIndex].key_token)) {
					if (oldIndex < source.entries.size() &&
						sets.entries[setIndex].key_token == source.entries[oldIndex].key_token) ++oldIndex;
					final.entries.push_back(std::move(sets.entries[setIndex++]));
				} else {
					auto &entry = source.entries[oldIndex++];
					if (!removals.contains(entry.key_token)) final.entries.push_back(std::move(entry));
				}
			}
			if (!constraintDefinitions.empty()) {
				if (!constraintSelection)
					constraintSelection = selectConstraintProperties(constraintDefinitions, resolvePropertyKey);
				const auto &selected = patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE
					? constraintSelection->node_tokens : constraintSelection->edge_tokens;
				std::vector<LogicalGraphWriteProperty> logical;
				logical.reserve(std::min(final.entries.size(), selected.size()));
				for (const auto &entry : final.entries) {
					if (readControl.checkpoint) readControl.checkpoint();
					// Only validator-owned fields belong in this transient view;
					// the full tokenized root remains the graph's authoritative state.
					if (!std::binary_search(selected.begin(), selected.end(), entry.key_token)) continue;
					const auto name = resolvePropertyName(entry.key_token);
					if (!name || name->empty()) fail(FormatError::FERR_INVALID_STATE, "native Property patch lost its token name");
					logical.push_back({*name, entry.value});
				}
				if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
					constraintBatch.node_creates.at(static_cast<size_t>(patch.entity.value)).properties = std::move(logical);
				else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE)
					constraintBatch.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties = std::move(logical);
				else constraintBatch.property_replacements.push_back({patch.entity, std::move(logical)});
			}
			if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE)
				nodes.at(static_cast<size_t>(patch.entity.value)).properties = std::move(final);
			else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE)
				createdEdgeProperties.emplace(patch.entity.value, std::move(final));
			else if (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE)
				persistedNodes[patch.entity.value].properties = std::move(final);
			else persistedEdges.emplace(patch.entity.value, std::move(final));
			debug::PerfTrace::addValue("native_graph.write.property_patch_targets", 1);
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
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			if (!request.detach) {
				continue;
			}
			for (const uint64_t edgeId:
				 transaction.collectIncidentEdgeIds(request.node_id, request.maximum_detach_edges)) {
				if (deletedEdges.insert(edgeId).second) {
					if (!constraintDefinitions.empty()) constraintBatch.edge_deletes.push_back({edgeId});
				}
			}
		}
		for (const uint64_t edgeId: deletedEdges) {
			if (topologyDeletedEdges.insert(edgeId).second) {
				transaction.eraseEdge(edgeId);
			}
		}

		const utils::OwnedStdVector<IndexDefinition> indexDefinitions = index_definitions_(snapshot);
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
		if (!constraintSelection)
			constraintSelection = selectConstraintProperties(constraintDefinitions, resolvePropertyKey);
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
				}, std::move(*constraintSelection),
				readControl.checkpoint, readControl.property_admission);
		ConstraintMutationValidator validator(
				constraintAdapter.makeAdapter(),
				{options_.maximum_constraint_entity_scan, options_.maximum_constraint_support_collisions});
		const std::vector<ConstraintSupportTreeMutationPlan> supportPlans =
				validator.validate(constraintDefinitions, constraintBatch);

        // The batch already knows its creation cardinality. Acquire one range
        // per entity kind instead of retaining one capability-map node per row.
        // This uses the same durable high-water protocol and bounded rollback.
        if (!batch.creation_identity_plan.has_value()) {
            if (readControl.checkpoint) readControl.checkpoint();
            transaction.reserveNodeIds(nodes.size());
            transaction.reserveEdgeIds(batch.edge_creates.size());
        }
        const auto reservedIdAt = [](std::span<const NativeGraphLogicalIdLeaseRange> ranges, size_t ordinal) {
            for (const auto &range : ranges) {
                if (ordinal < range.count) return range.first_id + ordinal;
                ordinal -= static_cast<size_t>(range.count);
            }
            throw FormatException(FormatError::FERR_INVALID_STATE, "native batch creation exceeds its ID reservation");
        };
		bool hasStagedWork = false;
		LogicalGraphMutationBatchResult result;
		result.created_node_ids.reserve(nodes.size());
		for (size_t ordinal = 0U; ordinal < nodes.size(); ++ordinal) {
			const uint64_t nodeId =
					batch.creation_identity_plan.has_value()
							? transaction.createNodeWithReservedId(batch.creation_identity_plan->node_ids.at(ordinal),
																   nodes[ordinal].labels)
							: transaction.createNodeWithReservedId(reservedIdAt(transaction.node_id_ranges_, ordinal), nodes[ordinal].labels);
			result.created_node_ids.push_back(nodeId);
			const PropertyCollectionRoot createdRoot = nodeRoot(nodeId, nodes[ordinal].properties.entries);
			stageVectorSemanticDelta(nodeId, {}, nullptr, nodes[ordinal].labels,
									 nodes[ordinal].properties.entries.empty() ? nullptr : &createdRoot);
			scalar.stageNodeLabels(transaction, nodeId, nodes[ordinal].labels, false);
			accountLabels(nodes[ordinal].labels, false);
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
			if (readControl.checkpoint) readControl.checkpoint();
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
			ResolvedProperties properties;
			if (const auto override = createdEdgeProperties.find(ordinal); override != createdEdgeProperties.end()) {
				properties = std::move(override->second);
			} else properties = resolveProperties(request.properties, resolvePropertyKey, readControl.checkpoint);
			const uint64_t edgeId =
					batch.creation_identity_plan.has_value()
							? transaction.createEdgeWithReservedId(batch.creation_identity_plan->edge_ids.at(ordinal),
															   sourceNodeId, targetNodeId,
															   typeToken)
							: transaction.createEdgeWithReservedId(reservedIdAt(transaction.edge_id_ranges_, ordinal),
                                    sourceNodeId, targetNodeId, typeToken);
			result.created_edge_ids.push_back(edgeId);
			scalar.stageEdgeType(transaction, edgeId, typeToken, false);
			accountToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, typeToken, false);
			if (!properties.entries.empty()) {
				PropertyCollectionRoot root = edgeRoot(edgeId, std::move(properties.entries));
				transaction.replaceEdgeProperty(root);
				scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {}, root, false);
			}
			hasStagedWork = true;
		}
		for (const auto &[nodeId, mutation]: persistedNodes) {
			const PageBackedGraphNode old = graph_.readNode(snapshot, nodeId, &oldStateReads);
			const std::vector<uint64_t> &newLabels = mutation.labels.has_value() ? *mutation.labels : old.label_token_ids;
			const std::vector<PropertyCollectionRootEntry> &newProperties =
					mutation.properties.has_value() ? mutation.properties->entries : old.properties;
			if (newLabels == old.label_token_ids && newProperties == old.properties) {
				continue;
			}
			const PropertyCollectionRoot oldRoot = nodeRoot(nodeId, old.properties);
			const PropertyCollectionRoot newRoot = nodeRoot(nodeId, newProperties);
			stageVectorSemanticDelta(nodeId, old.label_token_ids, &oldRoot, newLabels, &newRoot);
			if (newLabels != old.label_token_ids) {
				std::vector<uint64_t> removedLabels, addedLabels;
				std::ranges::set_difference(old.label_token_ids, newLabels, std::back_inserter(removedLabels));
				std::ranges::set_difference(newLabels, old.label_token_ids, std::back_inserter(addedLabels));
				scalar.stageNodeLabels(transaction, nodeId, removedLabels, true);
				accountLabels(removedLabels, true);
				scalar.stageNodeLabels(transaction, nodeId, addedLabels, false);
				accountLabels(addedLabels, false);
			}
			scalar.stagePropertyDelta(transaction, IndexDefinitionEntityKind::IDEK_NODE, nodeId,
				old.label_token_ids, oldRoot, newLabels, newRoot);
			if (newLabels != old.label_token_ids) {
				transaction.replaceNodeLabels(nodeId, newLabels);
			}
			if (newProperties != old.properties) {
				transaction.replaceNodeProperty(newRoot);
			}
			hasStagedWork = true;
		}
		for (const auto &[edgeId, properties]: persistedEdges) {
			const PageBackedGraphEdge old = graph_.readEdge(snapshot, edgeId, &oldStateReads);
			if (properties.entries == old.properties) {
				continue;
			}
			const PropertyCollectionRoot oldRoot = edgeRoot(edgeId, old.properties);
			const PropertyCollectionRoot newRoot = edgeRoot(edgeId, properties.entries);
			scalar.stagePropertyDelta(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {}, oldRoot, {}, newRoot);
			transaction.replaceEdgeProperty(newRoot);
			hasStagedWork = true;
		}

		// All explicit and DETACH Edge removals are already staged. eraseNode()
		// authenticates the remaining final topology and rejects incident Edges;
		// enumerating it here again would repeat the same two adjacency scans.
		std::vector<uint64_t> orderedErasedEdges(deletedEdges.begin(), deletedEdges.end());
		std::ranges::sort(orderedErasedEdges);
		for (const uint64_t edgeId: orderedErasedEdges) {
			const PageBackedGraphEdge old = graph_.readEdge(snapshot, edgeId, &oldStateReads);
			scalar.stageEdgeType(transaction, edgeId, old.type_id, true);
			accountToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, old.type_id, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_EDGE, edgeId, {},
								   edgeRoot(edgeId, old.properties), true);
			hasStagedWork = true;
		}
		// Request shape and detach bounds have already been validated in caller order.
		// Read immutable old state in ID order so directory/property cursors can
		// reuse their current page, as they do for property replacements and Edges.
		std::vector<uint64_t> orderedErasedNodes;
		orderedErasedNodes.reserve(batch.node_deletes.size());
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			orderedErasedNodes.push_back(request.node_id);
		}
		std::ranges::sort(orderedErasedNodes);
		for (const uint64_t nodeId: orderedErasedNodes) {
			const PageBackedGraphNode old = graph_.readNode(snapshot, nodeId, &oldStateReads);
			const PropertyCollectionRoot oldRoot = nodeRoot(nodeId, old.properties);
			stageVectorSemanticDelta(nodeId, old.label_token_ids, &oldRoot, {}, nullptr);
			scalar.stageNodeLabels(transaction, nodeId, old.label_token_ids, true);
			accountLabels(old.label_token_ids, true);
			scalar.stageProperties(transaction, IndexDefinitionEntityKind::IDEK_NODE, nodeId,
								   old.label_token_ids, oldRoot, true);
			transaction.eraseNode(nodeId);
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
			bootstrap.checkpoint = readControl.checkpoint;
			transaction.stageVectorIndexBootstrap(indexes_, vector_pages_, vector_manifests_, *definition, records,
												  bootstrap);
		}
		if (catalogSession.has_value()) {
			for (const auto &[key, delta] : tokenCountDeltas) {
				if (delta.added == delta.removed) continue;
				const auto current = system_catalog_->readTokenCardinality(snapshot, key.first, key.second);
				if (!current.has_value()) continue; // Historical unknown estimates remain unknown.
				const uint64_t additions = delta.added > delta.removed ? delta.added - delta.removed : 0U;
				const uint64_t removals = delta.removed > delta.added ? delta.removed - delta.added : 0U;
				catalogSession->setTokenCardinality(key.first, key.second,
						checkedCardinalityDelta(*current, additions, removals,
								"native token cardinality counter overflowed or underflowed"));
			}
		}
		if (catalogSession.has_value() && catalogSession->changed()) {
			catalogSession->finish();
			hasStagedWork = true;
		}
		if (readControl.checkpoint) readControl.checkpoint();
		if (!hasStagedWork) {
			if (batch.creation_identity_plan.has_value() && (!batch.creation_identity_plan->node_id_ranges.empty() ||
															 !batch.creation_identity_plan->edge_id_ranges.empty())) {
                finalizeCoveredAdjacency();
                transaction.native_graph_coordinated_ = true;
				return {std::move(result), transaction.commit()};
			}
			const Superblock base = transaction.baseSuperblock();
			transaction.rollback();
			return {std::move(result), {NativeGraphWriteCommitOutcome::NGWCO_COMMITTED, base, {}}};
		}
        finalizeCoveredAdjacency();
        transaction.native_graph_coordinated_ = true;
		const NativeGraphWriteCommitResult commit = transaction.commit();
		if (!commit.committed()) {
			result = {};
		}
		return {std::move(result), commit};
	}

} // namespace graph::storage
