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
#include <unordered_map>
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
									static_cast<uint8_t>(options.maximum_collisions == 0U) |
									static_cast<uint8_t>(options.validation_batch_entries == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
						"native constraint support-tree planning received invalid bounded placement options");
			}
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
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree planning could not resolve a Property-key Token");
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
				const OrderedBTreeEntrySource &source, size_t maximumCollisions, size_t batchEntries, NativeReadSession &session, const std::function<void(size_t)> &propertyAdmission) {
			OrderedBTreeEntryCursor cursor = source();
			session.bind(snapshot);
			std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> digest{};
			bool hasDigest = false;
			std::vector<std::vector<std::byte>> canonicalGroup;
			for (;;) {
				std::vector<BTreePageEntry> entries;
				entries.reserve(batchEntries);
				while (entries.size() < batchEntries) {
					const std::optional<BTreePageEntry> next = cursor();
					if (!next.has_value()) {
						break;
					}
					entries.push_back(*next);
				}
				if (entries.empty()) {
					break;
				}
				std::vector<std::pair<uint64_t, size_t>> orderedIds;
				orderedIds.reserve(entries.size());
				for (size_t ordinal = 0U; ordinal < entries.size(); ++ordinal) {
					const BTreePageEntry &entry = entries[ordinal];
					if (entry.key.size() != RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES || entry.value_or_child == 0U ||
							ConstraintSupportTree::entryEntityId(entry.key) != entry.value_or_child) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree plan produced an invalid Entity identity");
					}
					orderedIds.emplace_back(entry.value_or_child, ordinal);
				}
				std::ranges::sort(orderedIds);
				if (std::ranges::adjacent_find(orderedIds,
						[](const auto &left, const auto &right) { return left.first == right.first; }) != orderedIds.end()) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree plan repeated an Entity identity");
				}
				std::vector<uint64_t> ids;
				ids.reserve(orderedIds.size());
				for (const auto &[id, ordinal]: orderedIds) {
					static_cast<void>(ordinal);
					ids.push_back(id);
				}
				std::unordered_map<uint64_t, size_t> positions;
				positions.reserve(orderedIds.size());
				for (size_t position = 0U; position < orderedIds.size(); ++position) {
					positions.emplace(orderedIds[position].first, position);
				}
				std::vector<std::vector<std::optional<PropertyValue>>> selectedProperties;
				std::vector<std::vector<uint64_t>> labels;
				std::vector<EdgeEntityRecord> edgeRecords;
				// The Property-column API intentionally requires sorted tokens so it
				// can merge one ordered directory cursor. Constraint property order
				// remains authoritative for canonical-key construction below.
				std::vector<uint32_t> readPropertyTokens(propertyTokens.begin(), propertyTokens.end());
				std::ranges::sort(readPropertyTokens);
				if (std::ranges::adjacent_find(readPropertyTokens) != readPropertyTokens.end()) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree definition repeats a Property-key Token");
				}
				auto admission = makeSelectedPropertyBatchAdmission(propertyAdmission);
				if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
					labels = graph.readNodeLabelTokensSorted(snapshot, ids, &session);
					selectedProperties = graph.readNodePropertyColumnsSorted(snapshot, ids, readPropertyTokens, &session, admission ? &admission : nullptr);
					if (labels.size() != ids.size() ||
						selectedProperties.size() != propertyTokens.size()) {
						fail(FormatError::FERR_INVALID_STATE, "native constraint support-tree validation changed Node batch cardinality");
					}
				} else {
					edgeRecords = graph.readEdgeTopologiesSorted(snapshot, ids, &session);
					selectedProperties = graph.readEdgePropertyColumnsSorted(snapshot, ids, readPropertyTokens, &session, admission ? &admission : nullptr);
					if (edgeRecords.size() != ids.size() || selectedProperties.size() != propertyTokens.size()) {
						fail(FormatError::FERR_INVALID_STATE, "native constraint support-tree validation changed Edge batch cardinality");
					}
				}
				if (std::ranges::any_of(selectedProperties, [&ids](const auto &column) {
						return column.size() != ids.size();
					})) {
					fail(FormatError::FERR_INVALID_STATE,
							"native constraint support-tree validation changed Property column cardinality");
				}
				for (const BTreePageEntry &next: entries) {
					std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES> candidateDigest{};
					std::copy_n(next.key.begin(), candidateDigest.size(), candidateDigest.begin());
					if (!hasDigest || candidateDigest != digest) {
						digest = candidateDigest;
						hasDigest = true;
						canonicalGroup.clear();
					}
					if (canonicalGroup.size() >= maximumCollisions) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree digest group exceeded its collision bound");
					}
					const size_t position = positions.at(next.value_or_child);
					std::vector<PropertyCollectionRootEntry> properties;
					properties.reserve(propertyTokens.size());
					for (size_t token = 0U; token < readPropertyTokens.size(); ++token) {
						if (selectedProperties[token][position].has_value()) {
							properties.push_back({readPropertyTokens[token], *selectedProperties[token][position]});
						}
					}
					std::optional<std::vector<std::byte>> canonical;
				if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
					if (std::ranges::find(labels[position], definition.schema_token_id) == labels[position].end()) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree plan names a Node outside its schema scope");
					}
					canonical = canonicalValues(properties, propertyTokens, true);
				} else {
					if (edgeRecords[position].type_id != definition.schema_token_id) {
						fail(FormatError::FERR_INVALID_STATE,
								"native constraint support-tree plan names an Edge outside its schema scope");
					}
					canonical = canonicalValues(properties, propertyTokens, true);
				}
				if (!canonical.has_value() || !ConstraintSupportTree::matchesDigest(next.key, *canonical)) {
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
		ExternalBTreeEntrySorter sorter({options.maximum_entries, options.sort_run_bytes, options.maximum_sort_runs});
		std::vector<uint32_t> readTokens = propertyTokens;
		std::ranges::sort(readTokens);
		if (std::ranges::adjacent_find(readTokens) != readTokens.end()) {
			fail(FormatError::FERR_INVALID_STATE, "native constraint support-tree definition repeats a Property-key Token");
		}
		NativeReadSession session;
		const size_t batchEntries = std::min<size_t>(256U, options.validation_batch_entries);
		std::vector<uint64_t> ids;
		ids.reserve(std::min(batchEntries, options.maximum_entries));
		const bool node = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE;
		const auto flush = [&] {
			if (ids.empty()) return;
			if (options.checkpoint) options.checkpoint();
			auto admission = makeSelectedPropertyBatchAdmission(options.property_admission);
			auto columns = node ? graph.readNodePropertyColumnsSorted(snapshot, ids, readTokens, &session, admission ? &admission : nullptr)
				: graph.readEdgePropertyColumnsSorted(snapshot, ids, readTokens, &session, admission ? &admission : nullptr);
			if (columns.size() != readTokens.size() || std::ranges::any_of(columns, [&](const auto &column) {
				return column.size() != ids.size();
			})) fail(FormatError::FERR_INVALID_STATE, "native constraint support-tree source changed column cardinality");
			if (options.checkpoint) options.checkpoint();
			for (size_t row = 0U; row < ids.size(); ++row) {
				std::vector<PropertyCollectionRootEntry> properties;
				properties.reserve(readTokens.size());
				for (size_t column = 0U; column < readTokens.size(); ++column) {
					if (columns[column][row]) properties.push_back({readTokens[column], std::move(*columns[column][row])});
				}
				const auto canonical = canonicalValues(properties, propertyTokens,
					definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY);
				if (canonical) sorter.add({ConstraintSupportTree::entryKey(*canonical, ids[row]), ids[row]});
			}
			ids.clear();
		};
		size_t scannedForCheckpoint = 0U;
		const auto collect = [&](const EntityRef &ref, bool matches) {
			if (scannedForCheckpoint == options.maximum_entries)
				fail(FormatError::FERR_INVALID_STATE, "native constraint support-tree source exceeded its Entity scan bound");
			if (scannedForCheckpoint++ % 256U == 0U && options.checkpoint) options.checkpoint();
			if (matches) {
				ids.push_back(ref.entity_id);
				if (ids.size() == batchEntries) flush();
			}
			return true;
		};
		const std::array<uint64_t, 1U> labels{definition.schema_token_id};
		const size_t scanBound = options.maximum_entries == std::numeric_limits<size_t>::max()
			? options.maximum_entries : options.maximum_entries + 1U;
		static_cast<void>(node ? graph.visitNodeRefsWithLabelTokenMatchesUntil(snapshot, scanBound,
			labels, collect, &session) : graph.visitEdgeRefsUntil(snapshot, scanBound,
			[&](const EntityRef &ref) { return collect(ref, ref.type_token == definition.schema_token_id); }, &session));
		flush();
		const OrderedBTreeEntrySource uncheckedSource = sorter.finish();
		OrderedBTreeEntrySource source = uncheckedSource;
		if (options.checkpoint) source = [uncheckedSource, checkpoint = options.checkpoint] {
			auto cursor = uncheckedSource();
			return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable {
				if (count++ % 256U == 0U) checkpoint();
				return cursor();
			};
		};
		validateCanonicalEntries(definition, graph, snapshot, propertyTokens, source, options.maximum_collisions,
				options.validation_batch_entries, session, options.property_admission);
		return BulkBTreeBuilder::planNativeTree(
				pages, metadata, snapshot, definition.constraint_id, source,
				{options.first_slot, options.last_slot, definition.constraint_id, options.maximum_entries,
				 options.maximum_pages});
	}

} // namespace graph::storage
