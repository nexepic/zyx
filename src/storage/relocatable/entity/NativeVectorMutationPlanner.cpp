/**
 * @file NativeVectorMutationPlanner.cpp
 * @brief Snapshot-bound semantic Vector delta planning for native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateDefinitions(std::vector<IndexDefinition> &definitions, size_t maximumDefinitions) {
			if (maximumDefinitions == 0U || definitions.size() > maximumDefinitions) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Vector mutation planner requires a finite definition bound");
			}
			std::ranges::sort(definitions, {}, &IndexDefinition::index_id);
			uint64_t previousId = 0U;
		for (const IndexDefinition &definition: definitions) {
			static_cast<void>(IndexDefinitionCodec::encode(definition));
			if (previousId != 0U && definition.index_id == previousId) {
				fail(FormatError::FERR_INVALID_HEADER,
						 "native Vector mutation planner requires unique Index definitions");
			}
			if (definition.kind == IndexDefinitionKind::IDK_VECTOR &&
					definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native Vector mutation planner requires one bounded Node label/property Vector schema");
				}
				previousId = definition.index_id;
			}
		}

		void validateLabels(std::span<const uint64_t> labels) {
			if (std::ranges::any_of(labels, [](uint64_t label) { return label == 0U; }) ||
				!std::ranges::is_sorted(labels) || std::ranges::adjacent_find(labels) != labels.end()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Vector mutation planner requires canonical Node label Tokens");
			}
		}

		void validateProperties(uint64_t nodeId, const PropertyCollectionRoot *properties) {
			if (properties == nullptr) {
				return;
			}
			if (properties->owner_entity_id != nodeId || properties->owner_entity_type != Node::typeId) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native Vector mutation planner received a Property root with the wrong Node owner");
			}
			uint32_t previousToken = 0U;
			for (const PropertyCollectionRootEntry &entry: properties->entries) {
				if (entry.key_token == 0U || (previousToken != 0U && entry.key_token <= previousToken)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native Vector mutation planner requires canonical Property key Tokens");
				}
				previousToken = entry.key_token;
			}
		}

		[[nodiscard]] bool hasLabel(std::span<const uint64_t> labels, uint64_t labelToken) {
			return std::binary_search(labels.begin(), labels.end(), labelToken);
		}

		[[nodiscard]] const PropertyValue *findValue(const PropertyCollectionRoot *root, uint32_t token) {
			if (root == nullptr) {
				return nullptr;
			}
			const auto entry = std::lower_bound(root->entries.begin(), root->entries.end(), token,
											 [](const PropertyCollectionRootEntry &candidate, uint32_t expected) {
												 return candidate.key_token < expected;
											 });
			return entry != root->entries.end() && entry->key_token == token ? &entry->value : nullptr;
		}

	} // namespace

	NativeVectorMutationPlanner::NativeVectorMutationPlanner(utils::OwnedStdVector<IndexDefinition> definitions,
														 NativeVectorPropertyKeyTokenResolver propertyKeyTokens,
														 NativeVectorMutationPlannerOptions options) :
		definitions_(std::move(definitions)), property_key_tokens_(std::move(propertyKeyTokens)), options_(options) {
		if (!property_key_tokens_ || options_.maximum_mutations == 0U) {
			throw std::invalid_argument("native Vector mutation planner requires a Property-key resolver and finite bounds");
		}
		validateDefinitions(definitions_, options_.maximum_definitions);
	}

	std::optional<uint32_t>
	NativeVectorMutationPlanner::resolvePropertyKey(const MetadataSnapshotPin &snapshot, std::string_view name) const {
		const std::optional<uint32_t> token = property_key_tokens_(snapshot, name);
		if (token.has_value() && *token == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native Vector mutation planner Property-key resolver returned the zero Token");
		}
		return token;
	}

	std::vector<NativeVectorMutation>
	NativeVectorMutationPlanner::planNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
													 std::span<const uint64_t> oldLabels,
													 const PropertyCollectionRoot *oldProperties,
													 std::span<const uint64_t> newLabels,
													 const PropertyCollectionRoot *newProperties) const {
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native Vector mutation planner requires a nonzero Node ID");
		}
		validateLabels(oldLabels);
		validateLabels(newLabels);
		validateProperties(nodeId, oldProperties);
		validateProperties(nodeId, newProperties);

		std::vector<NativeVectorMutation> result;
		for (const IndexDefinition &definition: definitions_) {
			if (definition.kind != IndexDefinitionKind::IDK_VECTOR) {
				continue;
			}
			const std::optional<uint32_t> propertyToken = resolvePropertyKey(snapshot, definition.property_names.front());
			if (!propertyToken.has_value()) {
				// A valid Vector definition allocates its Property token in the same
				// schema publication. Reaching this branch would make membership
				// unknowable rather than safely empty.
				fail(FormatError::FERR_INVALID_STATE,
					 "native Vector mutation planner cannot resolve an indexed Property-key Token");
			}
			const PropertyValue *oldValue =
				hasLabel(oldLabels, definition.label_token_id) ? findValue(oldProperties, *propertyToken) : nullptr;
			const PropertyValue *newValue =
				hasLabel(newLabels, definition.label_token_id) ? findValue(newProperties, *propertyToken) : nullptr;
			if (oldValue == nullptr && newValue == nullptr) {
				continue;
			}
			const VectorDistanceMetric metric = VectorSemanticCodec::metricFor(definition);

			std::vector<std::byte> oldEncoded;
			std::vector<std::byte> newEncoded;
			if (oldValue != nullptr) {
				oldEncoded = VectorSemanticCodec::encodeBFloat16(*oldValue, definition.vector_dimensions);
				if (metric == VectorDistanceMetric::VDM_COSINE &&
					!VectorSemanticCodec::hasNonZeroFiniteL2Norm(oldEncoded)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native Vector mutation planner found a zero-norm cosine Vector property");
				}
			}
			if (newValue != nullptr) {
				newEncoded = VectorSemanticCodec::encodeBFloat16ForLogicalWrite(*newValue, definition.vector_dimensions);
				if (metric == VectorDistanceMetric::VDM_COSINE &&
					!VectorSemanticCodec::hasNonZeroFiniteL2Norm(newEncoded)) {
					throw std::invalid_argument("cosine Vector properties must have a nonzero finite L2 norm");
				}
			}
			if (oldValue != nullptr && newValue != nullptr && oldEncoded == newEncoded) {
				continue;
			}
			if (oldValue != nullptr) {
				result.push_back({NativeVectorMutationKind::NVMK_ERASE, definition.index_id, nodeId,
								  definition.vector_dimensions, metric, {}});
			}
			if (newValue != nullptr) {
				result.push_back({NativeVectorMutationKind::NVMK_INSERT, definition.index_id, nodeId,
								  definition.vector_dimensions, metric, std::move(newEncoded)});
			}
			if (result.size() > options_.maximum_mutations) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native Vector mutation planner exceeded its configured per-Node mutation bound");
			}
		}
		return result;
	}

} // namespace graph::storage
