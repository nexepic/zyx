/**
 * @file NativeScalarIndexMutationPlanner.cpp
 * @brief Native Entity-page scalar-index mutation planning.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateDefinitions(std::vector<IndexDefinition> &definitions) {
			std::ranges::sort(definitions, {}, &IndexDefinition::index_id);
			uint64_t previousId = 0U;
			for (const IndexDefinition &definition: definitions) {
				IndexDefinitionCodec::validate(definition);
				if (definition.index_id == 0U || (previousId != 0U && definition.index_id == previousId)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native scalar Index planner requires unique nonzero Index definitions");
				}
				if ((definition.kind == IndexDefinitionKind::IDK_PROPERTY ||
					 definition.kind == IndexDefinitionKind::IDK_COMPOSITE) &&
					definition.property_names.empty()) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native scalar Property Index definition requires at least one Property key");
				}
				previousId = definition.index_id;
			}
		}

		void validateCanonicalLabels(std::span<const uint64_t> labels) {
			if (std::ranges::any_of(labels, [](uint64_t value) { return value == 0U; }) ||
				!std::ranges::is_sorted(labels) || std::ranges::adjacent_find(labels) != labels.end()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native scalar Index planner requires strictly ordered nonzero Node labels");
			}
		}

		void validatePropertyRoot(IndexDefinitionEntityKind ownerKind, uint64_t ownerId,
								  const PropertyCollectionRoot &root) {
			const uint32_t expectedType =
					ownerKind == IndexDefinitionEntityKind::IDEK_NODE ? Node::typeId : Edge::typeId;
			if (ownerId == 0U || root.owner_entity_id != ownerId || root.owner_entity_type != expectedType) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native scalar Index planner received a Property root with the wrong direct owner");
			}
			uint32_t previous = 0U;
			for (const PropertyCollectionRootEntry &entry: root.entries) {
				if (entry.key_token == 0U || (previous != 0U && entry.key_token <= previous)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native scalar Index planner requires strictly ordered nonzero Property key Tokens");
				}
				previous = entry.key_token;
			}
		}

		[[nodiscard]] const PropertyValue *findPropertyValue(const PropertyCollectionRoot &root, uint32_t token) {
			const auto entry = std::lower_bound(root.entries.begin(), root.entries.end(), token,
												[](const PropertyCollectionRootEntry &candidate, uint32_t expected) {
													return candidate.key_token < expected;
												});
			return entry != root.entries.end() && entry->key_token == token ? &entry->value : nullptr;
		}

		[[nodiscard]] bool hasLabel(std::span<const uint64_t> labels, uint64_t labelId) {
			return std::binary_search(labels.begin(), labels.end(), labelId);
		}

		void validateSupportTree(const MetadataSnapshotPin &snapshot, RelocatableMetadataStore &metadata,
								 BTreePageStore &indexes, uint64_t constraintId,
								 std::span<const std::byte> canonicalKey, uint64_t entityId) {
			if (!isSystemCatalogConstraintDefinitionId(constraintId) || entityId == 0U || canonicalKey.empty() ||
				!indexes.usesMetadataStore(metadata)) {
				fail(FormatError::FERR_INVALID_HEADER, "native constraint support-tree mutation requires a local "
															   "System Catalog constraint and canonical Entity key");
			}
			if (!metadata.findIndexRootOptional(snapshot, constraintId).has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native active uniqueness constraint has no durable support-tree root");
			}
		}

	} // namespace

	NativeScalarIndexMutationPlanner::NativeScalarIndexMutationPlanner(
			RelocatableMetadataStore &metadata, BTreePageStore &indexes, utils::OwnedStdVector<IndexDefinition> definitions,
			NativePropertyKeyTokenResolver propertyKeyTokens) :
		metadata_(metadata), indexes_(indexes), definitions_(std::move(definitions)),
		property_key_tokens_(std::move(propertyKeyTokens)) {
		if (!indexes_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument("native scalar Index planner requires an Index store in its metadata domain");
		}
		if (!property_key_tokens_) {
			throw std::invalid_argument("native scalar Index planner requires a Property-key Token resolver");
		}
		validateDefinitions(definitions_);
	}

	bool NativeScalarIndexMutationPlanner::hasPublishedRoot(const MetadataSnapshotPin &snapshot,
																uint64_t indexId) const {
		const uint64_t databaseId = snapshot.superblock().database_id.value();
		const uint64_t generation = snapshot.superblock().generation.value();
		if (databaseId != published_root_cache_database_id_ ||
			generation != published_root_cache_generation_) {
			published_root_cache_.clear();
			published_root_cache_database_id_ = databaseId;
			published_root_cache_generation_ = generation;
		}
		if (const auto cached = published_root_cache_.find(indexId); cached != published_root_cache_.end()) {
			return cached->second;
		}
		const bool published = metadata_.findIndexRootOptional(snapshot, indexId).has_value();
		published_root_cache_.emplace(indexId, published);
		return published;
	}

	std::optional<uint32_t> NativeScalarIndexMutationPlanner::resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																				 std::string_view name) const {
		const std::optional<uint32_t> token = property_key_tokens_(snapshot, name);
		if (token.has_value() && *token == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native scalar Index planner Property-key resolver returned the zero Token");
		}
		return token;
	}

	void NativeScalarIndexMutationPlanner::stageNodeLabels(NativeGraphWriteTransaction &transaction, uint64_t nodeId,
														   std::span<const uint64_t> labels, bool erase) const {
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native scalar Index planner requires a nonzero Node ID");
		}
		validateCanonicalLabels(labels);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		for (const IndexDefinition &definition: definitions_) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind != IndexDefinitionKind::IDK_LABEL || !hasPublishedRoot(snapshot, definition.index_id)) {
				continue;
			}
			for (const uint64_t labelId: labels) {
				if (labelId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native scalar Index planner Node label Token exceeds its canonical scalar representation");
				}
				const std::vector<std::byte> key =
						IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(labelId)), nodeId);
				if (erase) {
					transaction.stageScalarIndexErase(indexes_, definition.index_id, key);
				} else {
					transaction.stageScalarIndexUpsert(indexes_, definition.index_id, key, nodeId);
				}
			}
		}
	}

	void NativeScalarIndexMutationPlanner::stageEdgeType(NativeGraphWriteTransaction &transaction, uint64_t edgeId,
														 uint64_t typeId, bool erase) const {
		if (edgeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native scalar Index planner requires a nonzero Edge ID");
		}
		if (typeId == RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN) {
			return;
		}
		if (typeId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native scalar Index planner Edge type Token exceeds its canonical scalar representation");
		}
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		for (const IndexDefinition &definition: definitions_) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_EDGE ||
				definition.kind != IndexDefinitionKind::IDK_LABEL || !hasPublishedRoot(snapshot, definition.index_id)) {
				continue;
			}
			const std::vector<std::byte> key =
					IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(typeId)), edgeId);
			if (erase) {
				transaction.stageScalarIndexErase(indexes_, definition.index_id, key);
			} else {
				transaction.stageScalarIndexUpsert(indexes_, definition.index_id, key, edgeId);
			}
		}
	}

	void NativeScalarIndexMutationPlanner::stageProperties(NativeGraphWriteTransaction &transaction,
														   IndexDefinitionEntityKind ownerKind, uint64_t ownerId,
														   std::span<const uint64_t> nodeLabels,
														   const PropertyCollectionRoot &root, bool erase) const {
		if (ownerKind != IndexDefinitionEntityKind::IDEK_NODE && ownerKind != IndexDefinitionEntityKind::IDEK_EDGE) {
			fail(FormatError::FERR_INVALID_HEADER, "native scalar Index planner has an unknown Property owner kind");
		}
		if (ownerKind == IndexDefinitionEntityKind::IDEK_NODE) {
			validateCanonicalLabels(nodeLabels);
		} else if (!nodeLabels.empty()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native scalar Index planner does not accept Node labels for an Edge Property root");
		}
		validatePropertyRoot(ownerKind, ownerId, root);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		for (const IndexDefinition &definition: definitions_) {
			if (definition.entity_kind != ownerKind ||
				(definition.kind != IndexDefinitionKind::IDK_PROPERTY &&
				 definition.kind != IndexDefinitionKind::IDK_COMPOSITE) ||
				!hasPublishedRoot(snapshot, definition.index_id)) {
				continue;
			}
			const auto key = propertyEntryKey(snapshot, definition, ownerId, nodeLabels, root);
			if (!key) continue;
			if (erase) {
				transaction.stageScalarIndexErase(indexes_, definition.index_id, *key);
			} else {
				transaction.stageScalarIndexUpsert(indexes_, definition.index_id, *key, ownerId);
			}
		}
	}

	std::optional<std::vector<std::byte>> NativeScalarIndexMutationPlanner::propertyEntryKey(
		const MetadataSnapshotPin &snapshot, const IndexDefinition &definition, uint64_t ownerId,
		std::span<const uint64_t> nodeLabels, const PropertyCollectionRoot &root) const {
        return propertyEntryKeyValues(snapshot, definition, ownerId, nodeLabels,
            [&](uint32_t token) { return findPropertyValue(root, token); });
    }

    std::optional<std::vector<std::byte>> NativeScalarIndexMutationPlanner::propertyEntryKeyValues(
        const MetadataSnapshotPin &snapshot, const IndexDefinition &definition, uint64_t ownerId,
        std::span<const uint64_t> nodeLabels, const PropertyValueLookup &lookup) const {
		if (definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE && definition.label_token_id != 0U &&
			!hasLabel(nodeLabels, definition.label_token_id)) return std::nullopt;
        if (definition.property_names.size() > RSS_INDEX_KEY_MAX_COMPONENTS)
            fail(FormatError::FERR_INVALID_HEADER, "relocatable composite index key requires a bounded nonempty component sequence");
        std::array<const PropertyValue *, RSS_INDEX_KEY_MAX_COMPONENTS> values{};
        size_t count = 0;
        for (const auto &name : definition.property_names) {
            const auto token = resolvePropertyKey(snapshot, name);
            if (!token) return std::nullopt;
            const auto *value = lookup(*token);
            if (!value) return std::nullopt;
            values[count++] = value;
        }
        return definition.kind == IndexDefinitionKind::IDK_PROPERTY
            ? IndexKeyCodec::tryEncodeEntryKey(*values[0], ownerId)
            : IndexKeyCodec::tryEncodeCompositeEntryKeyReferences(
                std::span<const PropertyValue *const>(values.data(), count), ownerId);
	}

    void NativeScalarIndexMutationPlanner::stagePropertyDelta(NativeGraphWriteTransaction &transaction,
        IndexDefinitionEntityKind ownerKind, uint64_t ownerId, std::span<const uint64_t> oldLabels,
        const PropertyCollectionRoot &oldRoot, std::span<const uint64_t> newLabels,
        const PropertyCollectionRoot &newRoot) const {
        visitPropertyDelta(transaction.baseSnapshot(), ownerKind, ownerId, oldLabels, oldRoot, newLabels, newRoot,
            [&](uint64_t indexId, std::span<const std::byte> key, uint64_t owner, bool erase) {
                if (erase) transaction.stageScalarIndexErase(indexes_, indexId, key);
                else transaction.stageScalarIndexUpsert(indexes_, indexId, key, owner);
            });
    }

    void NativeScalarIndexMutationPlanner::visitPropertyDelta(const MetadataSnapshotPin &snapshot,
        IndexDefinitionEntityKind ownerKind, uint64_t ownerId, std::span<const uint64_t> oldLabels,
        const PropertyCollectionRoot &oldRoot, std::span<const uint64_t> newLabels,
        const PropertyCollectionRoot &newRoot, const PropertyDeltaVisitor &visitor) const {
        if (!metadata_.ownsSnapshot(snapshot) || !visitor)
            fail(FormatError::FERR_INVALID_HEADER, "native scalar delta visitor requires its source pin and callback");
		if (ownerKind != IndexDefinitionEntityKind::IDEK_NODE && ownerKind != IndexDefinitionEntityKind::IDEK_EDGE)
			fail(FormatError::FERR_INVALID_HEADER, "native scalar Index planner has an unknown Property owner kind");
		if (ownerKind == IndexDefinitionEntityKind::IDEK_NODE) {
			validateCanonicalLabels(oldLabels);
			validateCanonicalLabels(newLabels);
		} else if (!oldLabels.empty() || !newLabels.empty()) {
			fail(FormatError::FERR_INVALID_HEADER, "native scalar Index planner does not accept Node labels for an Edge Property root");
		}
		validatePropertyRoot(ownerKind, ownerId, oldRoot);
		validatePropertyRoot(ownerKind, ownerId, newRoot);
        visitPropertyDeltaValues(snapshot, ownerKind, ownerId, oldLabels,
            [&](uint32_t token) { return findPropertyValue(oldRoot, token); }, newLabels,
            [&](uint32_t token) { return findPropertyValue(newRoot, token); }, visitor);
    }

    void NativeScalarIndexMutationPlanner::visitPropertyDeltaValues(const MetadataSnapshotPin &snapshot,
        IndexDefinitionEntityKind ownerKind, uint64_t ownerId, std::span<const uint64_t> oldLabels,
        const PropertyValueLookup &oldValues, std::span<const uint64_t> newLabels,
        const PropertyValueLookup &newValues, const PropertyDeltaVisitor &visitor) const {
        if (!metadata_.ownsSnapshot(snapshot) || !ownerId || !visitor || !oldValues || !newValues)
            fail(FormatError::FERR_INVALID_HEADER, "native scalar value delta requires its pin, owner and borrowed readers");
        if (ownerKind == IndexDefinitionEntityKind::IDEK_NODE) {
            validateCanonicalLabels(oldLabels); validateCanonicalLabels(newLabels);
        } else if (ownerKind != IndexDefinitionEntityKind::IDEK_EDGE || !oldLabels.empty() || !newLabels.empty())
            fail(FormatError::FERR_INVALID_HEADER, "native scalar value delta has an invalid owner kind or labels");
		for (const auto &definition : definitions_) {
			if (definition.entity_kind != ownerKind ||
				(definition.kind != IndexDefinitionKind::IDK_PROPERTY && definition.kind != IndexDefinitionKind::IDK_COMPOSITE) ||
				!hasPublishedRoot(snapshot, definition.index_id)) continue;
			const auto before = propertyEntryKeyValues(snapshot, definition, ownerId, oldLabels, oldValues);
			const auto after = propertyEntryKeyValues(snapshot, definition, ownerId, newLabels, newValues);
			// Byte identity handles codec equivalence and collisions consistently
			// with lookup, rather than relying on dynamic value equality.
			if (before == after) continue;
			if (before) visitor(definition.index_id, *before, ownerId, true);
			if (after) visitor(definition.index_id, *after, ownerId, false);
		}
	}

	void NativeScalarIndexMutationPlanner::stageConstraintSupportUpsert(NativeGraphWriteTransaction &transaction,
																		uint64_t constraintId,
																		std::span<const std::byte> canonicalKey,
																		uint64_t entityId) const {
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		validateSupportTree(snapshot, metadata_, indexes_, constraintId, canonicalKey, entityId);
		const std::vector<std::byte> key = ConstraintSupportTree::entryKey(canonicalKey, entityId);
		transaction.stageScalarIndexUpsert(indexes_, constraintId, key, entityId);
	}

	void NativeScalarIndexMutationPlanner::stageConstraintSupportErase(NativeGraphWriteTransaction &transaction,
																	   uint64_t constraintId,
																	   std::span<const std::byte> canonicalKey,
																	   uint64_t entityId) const {
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		validateSupportTree(snapshot, metadata_, indexes_, constraintId, canonicalKey, entityId);
		const std::vector<std::byte> key = ConstraintSupportTree::entryKey(canonicalKey, entityId);
		transaction.stageScalarIndexErase(indexes_, constraintId, key);
	}

} // namespace graph::storage
