/**
 * @file OfflineScalarIndexMaterializer.cpp
 * @brief Snapshot-pinned semantic scalar-index materialization implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/OfflineScalarIndexMaterializer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateMaterializationOptions(const OfflineScalarIndexMaterializationOptions &options) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_property_names == 0U) |
									static_cast<uint8_t>(options.build.maximum_entries == 0U) |
									static_cast<uint8_t>(options.build.maximum_pages == 0U) |
									static_cast<uint8_t>(options.build.maximum_entries != options.sort.maximum_entries);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline scalar-index materialization requires matching bounded sort and build capacities");
			}
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() ==
				std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE: metadata publication has the same LSN
														// exhaustion guard.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline scalar-index materialization cannot advance the destination checkpoint LSN");
			}
			return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] size_t verificationLimit(size_t maximumEntries) {
			if (maximumEntries ==
				std::numeric_limits<size_t>::max()) { // ZYX_COV_EXCL_LINE: bounded options cannot reach host maximum.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline scalar-index verification limit overflows host address space");
			}
			return maximumEntries + 1U;
		}

		[[nodiscard]] bool sameEntry(const BTreePageEntry &left, const BTreePageEntry &right) {
			return left.key == right.key && left.value_or_child == right.value_or_child;
		}

		struct TreeVerification {
			bool exists = false;
			uint64_t entry_count = 0;
			uint32_t semantic_crc = 0;
		};

		[[nodiscard]] TreeVerification verifyExisting(const MetadataSnapshotPin &snapshot, uint64_t indexId,
													  BTreePageStore &pages, RelocatableMetadataStore &metadata,
													  const OrderedBTreeEntrySource &expected, size_t maximumEntries) {
			if (!metadata.findIndexRootOptional(snapshot, indexId).has_value()) {
				return {};
			}
			const std::vector<BTreePageEntry> actual =
					BTreeIndex(indexId, pages, metadata)
							.scan(snapshot, std::nullopt, verificationLimit(maximumEntries));
			OrderedBTreeEntryCursor cursor = expected();
			if (actual.size() > maximumEntries) { // ZYX_COV_EXCL_LINE: scan limit is maximum + one.
				fail(FormatError::FERR_INVALID_STATE,
					 "offline scalar-index materialization existing tree exceeds its configured entry bound");
			}
			TreeVerification verification;
			verification.exists = true;
			for (const BTreePageEntry &entry: actual) {
				const std::optional<BTreePageEntry> candidate = cursor();
				if (!candidate.has_value() || !sameEntry(entry, *candidate)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline scalar-index materialization existing root disagrees with migrated semantics");
				}
				const std::vector<std::byte> encoded = BTreePageEntryCodec::encode(entry);
				verification.semantic_crc = utils::updateCrc(verification.semantic_crc, encoded.data(), encoded.size());
				++verification.entry_count;
			}
			if (cursor().has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline scalar-index materialization existing tree is missing migrated semantic entries");
			}
			return verification;
		}

		void addSemanticTree(OfflineScalarIndexMaterializationResult &result, uint64_t indexId,
							 const TreeVerification &tree) {
			if (result.semantic_index_count ==
						std::numeric_limits<uint64_t>::max() || // ZYX_COV_EXCL_LINE: materialized definition and entry
																// vectors cannot reach the 64-bit aggregate limit.
				result.semantic_entry_count > std::numeric_limits<uint64_t>::max() - tree.entry_count) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline scalar-index materialization semantic summary overflowed");
			}
			constexpr uint8_t SCALAR_INDEX_SEMANTIC_TAG = 10U;
			std::array<std::byte, sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t)> bytes{};
			LittleEndianCodec::writeU64(bytes, 0, indexId);
			LittleEndianCodec::writeU64(bytes, sizeof(uint64_t), tree.entry_count);
			LittleEndianCodec::writeU32(bytes, sizeof(uint64_t) * 2U, tree.semantic_crc);
			result.semantic_crc = utils::updateCrc(result.semantic_crc, &SCALAR_INDEX_SEMANTIC_TAG,
												   sizeof(SCALAR_INDEX_SEMANTIC_TAG));
			result.semantic_crc = utils::updateCrc(result.semantic_crc, bytes.data(), bytes.size());
			++result.semantic_index_count;
			result.semantic_entry_count += tree.entry_count;
		}

		[[nodiscard]] bool nodeHasLabel(std::span<const uint64_t> labels, uint64_t labelId) {
			return std::ranges::binary_search(labels, labelId);
		}

		void validatePropertyOwner(const PropertyCollectionRoot &root, const EntityPropertyKey &key,
								   const IndexDefinition &definition) {
			const uint64_t expectedType =
					definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE ? Node::typeId : Edge::typeId;
			const uint8_t invalid = static_cast<uint8_t>(root.owner_entity_id != key.entity_id) |
									static_cast<uint8_t>(root.owner_entity_type != expectedType);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline scalar-index owner-keyed Property root disagrees with its Entity Map owner");
			}
		}

		[[nodiscard]] const PropertyValue *findValue(const PropertyCollectionRoot &root, uint32_t token) {
			const auto entry =
					std::ranges::lower_bound(root.entries, token, {}, &PropertyCollectionRootEntry::key_token);
			return entry == root.entries.end() || entry->key_token != token
						   ? nullptr
						   : &entry->value; // ZYX_COV_EXCL_LINE: caller tests presence with a canonical token.
		}

		[[nodiscard]] std::optional<std::vector<uint32_t>> resolvePropertyTokens(
				const IndexDefinition &definition, const SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
				size_t maximumPropertyNames) {
			if (definition.property_names.size() > maximumPropertyNames) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline scalar-index materialization definition exceeds its configured Property-name bound");
			}
			std::vector<uint32_t> tokens;
			tokens.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = catalog.findPropertyToken(snapshot, name);
				if (!token.has_value()) {
					return std::nullopt;
				}
				tokens.push_back(*token);
			}
			return tokens;
		}

		void collectLabelEntries(const IndexDefinition &definition, RelocatableMetadataStore &metadata,
								 EntityPageStore &entities, EntityLabelSetStore &labels,
								 const MetadataSnapshotPin &snapshot, ExternalBTreeEntrySorter &sorter) {
			if (definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE) {
				metadata.visitNodePageLocations(snapshot, [&](const EntityPageLocation &location) {
					const NodeEntityRecord node = entities.readNodeAt(snapshot, location);
					for (const uint64_t labelId: labels.readNodeLabels(snapshot, node)) {
						if (labelId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							fail(FormatError::FERR_INVALID_STATE,
								 "offline scalar-index Node has an invalid native label token");
						}
						sorter.add({IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(labelId)),
																  location.entity_id),
									location.entity_id});
					}
				});
				return;
			}
			metadata.visitEdgePageLocations(snapshot, [&](const EntityPageLocation &location) {
				const EdgeEntityRecord edge = entities.readEdgeAt(snapshot, location);
				if (edge.type_id == RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN) {
					// Type indexes are defined over named schema tokens only.
					return;
				}
				if (edge.type_id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					fail(FormatError::FERR_INVALID_STATE, "offline scalar-index Edge has an invalid native type token");
				}
				sorter.add({IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(edge.type_id)),
														  location.entity_id),
							location.entity_id});
			});
		}

		void collectPropertyEntries(const IndexDefinition &definition, const std::vector<uint32_t> &tokens,
									RelocatableMetadataStore &metadata, EntityPageStore &entities,
									EntityLabelSetStore &labels, PropertyCollectionRootStore &properties,
									const MetadataSnapshotPin &snapshot, ExternalBTreeEntrySorter &sorter) {
			const auto addRoot = [&definition, &tokens, &sorter](const PropertyCollectionRoot &root) {
				std::vector<PropertyValue> values;
				values.reserve(tokens.size());
				for (const uint32_t token: tokens) {
					const PropertyValue *value = findValue(root, token);
					if (value == nullptr) {
						return;
					}
					values.push_back(*value);
				}
				const std::optional<std::vector<std::byte>> key =
						definition.kind == IndexDefinitionKind::IDK_PROPERTY
								? IndexKeyCodec::tryEncodeEntryKey(values.front(), root.owner_entity_id)
								: IndexKeyCodec::tryEncodeCompositeEntryKey(values, root.owner_entity_id);
				if (key.has_value()) {
					sorter.add({std::move(*key), root.owner_entity_id});
				}
			};
			PropertyCollectionRootScanCursor propertyCursor = properties.openScanCursor(snapshot);
			if (definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE) {
				metadata.visitNodePageLocations(snapshot, [&](const EntityPageLocation &location) {
					const NodeEntityRecord node = entities.readNodeAt(snapshot, location);
					const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, location.entity_id};
					const std::optional<PropertyCollectionRoot> root = propertyCursor.readEntityOptional(snapshot, key);
					if (!root.has_value()) {
						return;
					}
					validatePropertyOwner(*root, key, definition);
					if (definition.label_token_id != 0U &&
						!nodeHasLabel(labels.readNodeLabels(snapshot, node), definition.label_token_id)) {
						return;
					}
					addRoot(*root);
				});
				return;
			}
			metadata.visitEdgePageLocations(snapshot, [&](const EntityPageLocation &location) {
				static_cast<void>(entities.readEdgeAt(snapshot, location));
				const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_EDGE, location.entity_id};
				const std::optional<PropertyCollectionRoot> root = propertyCursor.readEntityOptional(snapshot, key);
				if (!root.has_value()) {
					return;
				}
				validatePropertyOwner(*root, key, definition);
				addRoot(*root);
			});
		}

	} // namespace

	void OfflineScalarIndexMaterializer::validateOptions(const OfflineScalarIndexMaterializationOptions &options) {
		validateMaterializationOptions(options);
	}

	OfflineScalarIndexMaterializationResult OfflineScalarIndexMaterializer::materialize(
			std::span<const IndexDefinition> definitions, BTreePageStore &pages, EntityPageStore &entities,
			EntityLabelSetStore &labels, PropertyCollectionRootStore &properties,
			SystemCatalog &catalog, RelocatableMetadataStore &metadata,
			const OfflineScalarIndexMaterializationOptions &options) {
		validateMaterializationOptions(options);
		if (definitions.size() > options.maximum_definitions || !pages.usesMetadataStore(metadata) ||
			!entities.usesMetadataStore(metadata) || !labels.usesMetadataStore(metadata) ||
			!properties.usesMetadataStore(metadata) ||
			!catalog.usesMetadataStore(metadata)) { // ZYX_COV_EXCL_LINE: destination stores are paired at construction.
			fail(FormatError::FERR_INVALID_STATE,
				 "offline scalar-index materialization has too many definitions or inconsistent destination stores");
		}

		MetadataSnapshotPin sourceSnapshot = metadata.openSnapshot();
		OfflineScalarIndexMaterializationResult result;
		for (const IndexDefinition &definition: definitions) {
			static_cast<void>(IndexDefinitionCodec::encode(definition));
			if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
				++result.deferred_vector_indexes;
				continue;
			}
			ExternalBTreeEntrySorter sorter(options.sort);
			bool assignedPropertyTokens = true;
			if (definition.kind == IndexDefinitionKind::IDK_LABEL) {
				collectLabelEntries(definition, metadata, entities, labels, sourceSnapshot, sorter);
			} else {
				const std::optional<std::vector<uint32_t>> propertyTokens =
						resolvePropertyTokens(definition, catalog, sourceSnapshot, options.maximum_property_names);
				if (!propertyTokens.has_value()) {
					assignedPropertyTokens = false;
				} else {
					collectPropertyEntries(definition, *propertyTokens, metadata, entities, labels, properties, sourceSnapshot,
										   sorter);
				}
			}
			if (!assignedPropertyTokens) {
				++result.empty_indexes_with_unassigned_property_keys;
			}

			const OrderedBTreeEntrySource source = sorter.finish();
			MetadataSnapshotPin existingSnapshot = metadata.openSnapshot();
			const TreeVerification existing = verifyExisting(existingSnapshot, definition.index_id, pages, metadata,
															 source, options.build.maximum_entries);
			existingSnapshot.release();
			if (existing.exists) {
				++result.verified_indexes;
				addSemanticTree(result, definition.index_id, existing);
				continue;
			}
			const BulkBTreeBuildResult built = BulkBTreeBuilder::build(pages, metadata, definition.index_id, source,
																	   options.build, nextCheckpointLsn(metadata));
			result.indexed_entries += built.entry_count;
			++result.materialized_indexes;
			MetadataSnapshotPin verificationSnapshot = metadata.openSnapshot();
			const TreeVerification published = verifyExisting(verificationSnapshot, definition.index_id, pages,
															  metadata, source, options.build.maximum_entries);
			if (!published
						 .exists) { // ZYX_COV_EXCL_LINE: bulk publication makes the verified root reachable atomically.
				fail(FormatError::FERR_INVALID_STATE,
					 "offline scalar-index materialization did not publish its new Index Root Directory entry");
			}
			verificationSnapshot.release();
			addSemanticTree(result, definition.index_id, published);
		}
		sourceSnapshot.release();
		return result;
	}

} // namespace graph::storage
