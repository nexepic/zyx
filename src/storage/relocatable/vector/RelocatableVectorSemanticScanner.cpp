/**
 * @file RelocatableVectorSemanticScanner.cpp
 * @brief Bounded relocatable Vector semantic extraction implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/RelocatableVectorSemanticScanner.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr uint8_t VECTOR_SEMANTIC_TAG = 11U;
		constexpr size_t MAX_VECTOR_BATCH_RECORDS = 65536U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const RelocatableVectorSemanticScanOptions &options) {
			const uint8_t invalid =
					static_cast<uint8_t>(options.maximum_definitions == 0U) |
					static_cast<uint8_t>(options.maximum_batch_records == 0U) |
					static_cast<uint8_t>(options.maximum_batch_records > MAX_VECTOR_BATCH_RECORDS) |
					static_cast<uint8_t>(options.maximum_property_names == 0U) |
					static_cast<uint8_t>(options.maximum_vectors_per_index == 0U) |
					static_cast<uint8_t>(options.maximum_dimensions == 0U) |
					static_cast<uint8_t>(options.maximum_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector semantic scan requires bounded definitions, "
													   "batches, vector counts, and dimensions");
			}
		}

		[[nodiscard]] bool nodeHasLabel(std::span<const uint64_t> labels, uint64_t labelId) {
			return std::ranges::binary_search(labels, labelId);
		}

		[[nodiscard]] const PropertyValue *findValue(const PropertyCollectionRoot &root, uint32_t token) {
			const auto entry =
					std::ranges::lower_bound(root.entries, token, {}, &PropertyCollectionRootEntry::key_token);
			return entry == root.entries.end() || entry->key_token != token ? nullptr : &entry->value;
		}

		void updateSemantic(RelocatableVectorSemanticScanResult &result,
							const RelocatableVectorSemanticRecord &record) {
			if (result.vector_count == std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE: bounded source
																			   // scans cannot emit 2^64 records.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable Vector semantic record count overflowed");
			}
			std::array<std::byte, sizeof(uint64_t) * 2U> identity{};
			LittleEndianCodec::writeU64(identity, 0, record.index_id);
			LittleEndianCodec::writeU64(identity, sizeof(uint64_t), record.node_id);
			result.semantic_crc =
					utils::updateCrc(result.semantic_crc, &VECTOR_SEMANTIC_TAG, sizeof(VECTOR_SEMANTIC_TAG));
			result.semantic_crc = utils::updateCrc(result.semantic_crc, identity.data(), identity.size());
			result.semantic_crc =
					utils::updateCrc(result.semantic_crc, record.raw_bfloat16.data(), record.raw_bfloat16.size());
			++result.vector_count;
		}

	} // namespace

	RelocatableVectorSemanticScanResult
	RelocatableVectorSemanticScanner::scan(std::span<const IndexDefinition> definitions, EntityPageStore &entities,
									   EntityLabelSetStore &labels, PropertyCollectionRootStore &properties,
									   SystemCatalog &catalog, RelocatableMetadataStore &metadata,
									   const RelocatableVectorSemanticScanOptions &options,
										   const RelocatableVectorSemanticBatchConsumer &consumer) {
		validateOptions(options);
		if (!consumer || definitions.size() > options.maximum_definitions || !entities.usesMetadataStore(metadata) ||
			!labels.usesMetadataStore(metadata) || !properties.usesMetadataStore(metadata) ||
			!catalog.usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector semantic scan requires one metadata domain, bounded definitions, and a consumer");
		}

		MetadataSnapshotPin snapshot = metadata.openSnapshot();

		RelocatableVectorSemanticScanResult result;
		for (const IndexDefinition &definition: definitions) {
			if (definition.kind != IndexDefinitionKind::IDK_VECTOR) {
				continue;
			}
			static_cast<void>(IndexDefinitionCodec::encode(definition));
			if (definition.vector_dimensions > options.maximum_dimensions) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector semantic scan definition exceeds its configured raw-page dimension bound");
			}
			if (definition.property_names.size() > options.maximum_property_names) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector semantic scan definition exceeds its configured Property-name bound");
			}
			const std::optional<uint32_t> token = catalog.findPropertyToken(snapshot, definition.property_names.front());
			if (!token.has_value()) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable Vector semantic scan definition references a "
													  "Property key absent from the System Catalog");
			}
			uint64_t vectorsForIndex = 0;
			uint64_t previousNodeId = 0;
			bool hasPreviousNode = false;
			std::vector<RelocatableVectorSemanticRecord> batch;
			batch.reserve(options.maximum_batch_records);
			PropertyCollectionRootScanCursor propertyCursor = properties.openScanCursor(snapshot);
			metadata.visitNodePageLocations(snapshot, [&](const EntityPageLocation &location) {
				if (hasPreviousNode && location.entity_id <= previousNodeId) { // ZYX_COV_EXCL_LINE: LocationDirectory
																			   // emits strictly ordered logical IDs.
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector semantic scan Node Directory order is not canonical");
				}
				previousNodeId = location.entity_id;
				hasPreviousNode = true;
				const NodeEntityRecord node = entities.readNodeAt(snapshot, location);
				if (!nodeHasLabel(labels.readNodeLabels(snapshot, node), definition.label_token_id)) {
					return;
				}
				const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, location.entity_id};
				const std::optional<PropertyCollectionRoot> root =
						propertyCursor.readEntityOptional(snapshot, propertyKey);
				if (!root.has_value()) {
					return;
				}
				if (root->owner_entity_type != Node::typeId || root->owner_entity_id != location.entity_id) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector semantic scan Property root disagrees with native Node ownership");
				}
				const PropertyValue *value = findValue(*root, *token);
				if (value == nullptr) {
					return;
				}
				if (vectorsForIndex == options.maximum_vectors_per_index) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector semantic scan exceeded its configured per-index vector bound");
				}
				RelocatableVectorSemanticRecord record;
				record.index_id = definition.index_id;
				record.node_id = location.entity_id;
				record.raw_bfloat16 = VectorSemanticCodec::encodeBFloat16(*value, definition.vector_dimensions);
				updateSemantic(result, record);
				batch.push_back(std::move(record));
				++vectorsForIndex;
				if (batch.size() == options.maximum_batch_records) {
					consumer(batch);
					batch.clear();
				}
			});
			if (!batch.empty()) {
				consumer(batch);
			}
		}
		snapshot.release();
		return result;
	}

} // namespace graph::storage
