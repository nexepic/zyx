/**
 * @file NativeConstraintSupportTreeBuilder.cpp
 * @brief Snapshot-pinned native construction plans for constraint support trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#include "graph/storage/relocatable/schema/NativeConstraintSupportTreeBuilder.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <vector>

#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool requiresSupportTree(const ConstraintDefinition &definition) {
			return definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
				   definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		}

		void validateOptions(const NativeConstraintSupportTreeBuildOptions &options) {
			const uint8_t invalid = static_cast<uint8_t>(!options.first_slot.isValid()) |
										static_cast<uint8_t>(!options.last_slot.isValid()) |
										static_cast<uint8_t>(options.first_slot > options.last_slot) |
										static_cast<uint8_t>(options.maximum_entries == 0U) |
										static_cast<uint8_t>(options.maximum_pages == 0U) |
										static_cast<uint8_t>(options.sort_run_bytes == 0U) |
										static_cast<uint8_t>(options.maximum_sort_runs < 2U) |
										static_cast<uint8_t>(options.maximum_collisions == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
						"native constraint support-tree planning received invalid bounded placement options");
			}
		}

		[[nodiscard]] bool nodeInScope(const PageBackedGraphNode &node, const ConstraintDefinition &definition) {
			return std::ranges::find(node.label_token_ids, definition.schema_token_id) != node.label_token_ids.end();
		}

		[[nodiscard]] bool edgeInScope(const PageBackedGraphEdge &edge, const ConstraintDefinition &definition) {
			return edge.type_id == definition.schema_token_id;
		}

		[[nodiscard]] const PropertyValue *findProperty(const std::vector<PropertyCollectionRootEntry> &entries,
				uint32_t token) {
			const auto entry = std::ranges::lower_bound(entries, token, {}, &PropertyCollectionRootEntry::key_token);
			return entry == entries.end() || entry->key_token != token ? nullptr : &entry->value;
		}

		[[nodiscard]] std::vector<uint32_t> resolvePropertyTokens(
				const MetadataSnapshotPin &snapshot, const ConstraintDefinition &definition,
				const NativeConstraintPropertyTokenResolver &propertyTokenResolver) {
			std::vector<uint32_t> tokens;
			tokens.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = propertyTokenResolver(snapshot, name);
				if (!token.has_value()) {
					tokens.clear();
					return tokens;
				}
				if (*token == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native constraint support-tree planning resolved a zero Property-key Token");
				}
				tokens.push_back(*token);
			}
			return tokens;
		}

		[[nodiscard]] std::optional<std::vector<std::byte>> canonicalValues(
				const std::vector<PropertyCollectionRootEntry> &properties, std::span<const uint32_t> tokens,
				bool requireComplete) {
			std::vector<PropertyValue> values;
			values.reserve(tokens.size());
			for (const uint32_t token: tokens) {
				const PropertyValue *value = findProperty(properties, token);
				if (value == nullptr || value->getType() == PropertyType::NULL_TYPE) {
					if (requireComplete) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree Node KEY has a missing or null Property");
					}
					return std::nullopt;
				}
				values.push_back(*value);
			}
			return ConstraintSupportTree::canonicalKey(values);
		}

		void validateCanonicalEntries(const ConstraintDefinition &definition, PageBackedGraphReadAdapter &graph,
				const MetadataSnapshotPin &snapshot, std::span<const uint32_t> propertyTokens,
				const OrderedBTreeEntrySource &source, size_t maximumCollisions) {
			OrderedBTreeEntryCursor cursor = source();
			std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> digest{};
			bool hasDigest = false;
			std::vector<std::vector<std::byte>> canonicalGroup;
			for (;;) {
				const std::optional<BTreePageEntry> next = cursor();
				if (!next.has_value()) {
					break;
				}
				if (next->key.size() != RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES || next->value_or_child == 0U ||
						ConstraintSupportTree::entryEntityId(next->key) != next->value_or_child) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree plan produced an invalid Entity identity");
				}
				std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> candidateDigest{};
				std::copy_n(next->key.begin(), candidateDigest.size(), candidateDigest.begin());
				if (!hasDigest || candidateDigest != digest) {
					digest = candidateDigest;
					hasDigest = true;
					canonicalGroup.clear();
				}
				if (canonicalGroup.size() >= maximumCollisions) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree digest group exceeded its collision bound");
				}
				std::optional<std::vector<std::byte>> canonical;
				if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
					const PageBackedGraphNode node = graph.readNode(snapshot, next->value_or_child);
					if (!nodeInScope(node, definition)) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree plan names a Node outside its schema scope");
					}
					canonical = canonicalValues(node.properties, propertyTokens, true);
				} else {
					const PageBackedGraphEdge edge = graph.readEdge(snapshot, next->value_or_child);
					if (!edgeInScope(edge, definition)) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree plan names an Edge outside its schema scope");
					}
					canonical = canonicalValues(edge.properties, propertyTokens, true);
				}
				if (!canonical.has_value() || !ConstraintSupportTree::matchesDigest(next->key, *canonical)) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree digest disagrees with canonical Property values");
				}
				if (std::ranges::find(canonicalGroup, *canonical) != canonicalGroup.end()) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree found duplicate uniqueness semantics");
				}
				canonicalGroup.push_back(std::move(*canonical));
			}
		}

	} // namespace

	BTreeInitialTreeMutationPlan NativeConstraintSupportTreeBuilder::plan(
			BTreePageStore &pages, RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
			const ConstraintDefinition &definition, PageBackedGraphReadAdapter &graph,
			const NativeConstraintSupportTreeBuildOptions &options,
			const NativeConstraintPropertyTokenResolver &propertyTokenResolver) {
		validateOptions(options);
		if (!snapshot.isValid() || !pages.usesMetadataStore(metadata) || !propertyTokenResolver ||
				!requiresSupportTree(definition) ||
				!isSystemCatalogConstraintDefinitionId(definition.constraint_id)) {
			fail(FormatError::FERR_INVALID_HEADER,
					"native constraint support-tree planning received an invalid definition or metadata domain");
		}
		if (metadata.findIndexRootOptional(snapshot, definition.constraint_id).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
					"native constraint support-tree planning cannot replace an existing support-tree root");
		}

		const std::vector<uint32_t> propertyTokens = resolvePropertyTokens(snapshot, definition, propertyTokenResolver);
		const bool propertyKeysAssigned = propertyTokens.size() == definition.property_names.size();
		ExternalBTreeEntrySorter sorter({options.maximum_entries, options.sort_run_bytes, options.maximum_sort_runs});
		bool scopedEntityExists = false;
		const auto add = [&](uint64_t entityId, const std::vector<uint64_t> &labels, uint64_t typeId,
				const std::vector<PropertyCollectionRootEntry> &properties) {
			const bool inScope = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
						? std::ranges::find(labels, definition.schema_token_id) != labels.end()
						: typeId == definition.schema_token_id;
			if (!inScope) {
				return;
			}
			scopedEntityExists = true;
			if (!propertyKeysAssigned) {
				if (definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree cannot build a Node KEY with an unassigned Property key");
				}
				return;
			}
			const std::optional<std::vector<std::byte>> canonical = canonicalValues(
					properties, propertyTokens, definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY);
			if (!canonical.has_value()) {
				return;
			}
			sorter.add({ConstraintSupportTree::entryKey(*canonical, entityId), entityId});
		};

		if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
			static_cast<void>(graph.visitNodesUntil(snapshot, options.maximum_entries,
					[&](const PageBackedGraphNode &node) {
						add(node.node_id, node.label_token_ids, 0U, node.properties);
						return true;
					}));
		} else {
			static_cast<void>(graph.visitEdgesUntil(snapshot, options.maximum_entries,
					[&](const PageBackedGraphEdge &edge) {
						add(edge.edge_id, {}, edge.type_id, edge.properties);
						return true;
					}));
		}
		if (!propertyKeysAssigned && definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY && scopedEntityExists) {
			fail(FormatError::FERR_INVALID_STATE,
					"native constraint support-tree cannot build a Node KEY with an unassigned Property key");
		}

		const OrderedBTreeEntrySource source = sorter.finish();
		if (propertyKeysAssigned) {
			validateCanonicalEntries(definition, graph, snapshot, propertyTokens, source, options.maximum_collisions);
		}
		return BulkBTreeBuilder::planNativeTree(
				pages, metadata, snapshot, definition.constraint_id, source,
				{options.first_slot, options.last_slot, definition.constraint_id, options.maximum_entries,
				 options.maximum_pages});
	}

} // namespace graph::storage
