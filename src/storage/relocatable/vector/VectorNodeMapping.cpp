/**
 * @file VectorNodeMapping.cpp
 * @brief Immutable VectorId-to-NodeId B+Tree contract implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

#include <limits>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		constexpr uint64_t RSS_VECTOR_NODE_MAPPING_ID_SUFFIX_MASK = UINT64_C(0x0000FFFFFFFFFFFF);
		constexpr uint64_t RSS_VECTOR_NODE_MAPPING_ID_SALT = UINT64_C(0xD6E8FEB86659FD93);
		constexpr size_t RSS_VECTOR_NODE_MAPPING_MAX_ID_PROBES = 256U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint64_t mixId(uint64_t value) {
			value += UINT64_C(0x9E3779B97F4A7C15);
			value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
			value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
			return value ^ (value >> 31U);
		}

		[[nodiscard]] uint64_t candidateId(uint64_t vectorIndexId, size_t probe) {
			return RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX |
				   (mixId(vectorIndexId ^ RSS_VECTOR_NODE_MAPPING_ID_SALT ^ static_cast<uint64_t>(probe)) &
					RSS_VECTOR_NODE_MAPPING_ID_SUFFIX_MASK);
		}

		[[nodiscard]] uint64_t decodeKey(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector-to-Node mapping key does not have the canonical Vector-ID width");
			}
			uint64_t value = 0U;
			for (const std::byte byte: key) {
				value = (value << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(byte));
			}
			return value;
		}

		void validateBuildInput(uint64_t mappingIndexId, const OrderedBTreeEntrySource &source,
								const VectorNodeMappingBuildOptions &options, LogSequenceNumber checkpointLsn) {
			VectorNodeMapping::validateMappingIndexId(mappingIndexId);
			const uint8_t invalid = static_cast<uint8_t>(!source) |
								static_cast<uint8_t>(options.maximum_entries == 0U) |
								static_cast<uint8_t>(options.tree.maximum_entries != options.maximum_entries) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector-to-Node mapping requires bounded nonempty entries and a checkpoint LSN");
			}
			size_t count = 0U;
			uint64_t previousVectorId = 0U;
			OrderedBTreeEntryCursor cursor = source();
			while (const std::optional<BTreePageEntry> entry = cursor()) {
				const uint64_t vectorId = decodeKey(entry->key);
				if (vectorId == 0U || entry->value_or_child == 0U || vectorId <= previousVectorId ||
					count == options.maximum_entries) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable Vector-to-Node mapping entries must have strictly ordered nonzero identities");
				}
				previousVectorId = vectorId;
				++count;
			}
			if (count == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector-to-Node mapping requires at least one semantic entry");
			}
		}

	} // namespace

	void VectorNodeMapping::validateMappingIndexId(uint64_t mappingIndexId) {
		if (!isMappingIndexId(mappingIndexId)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector-to-Node mapping requires a metadata-owned typed derived Index ID");
		}
	}

	uint64_t VectorNodeMapping::selectMappingIndexId(const MetadataSnapshotPin &snapshot, RelocatableMetadataStore &metadata,
														 uint64_t vectorIndexId) {
		if (!snapshot.isValid() || vectorIndexId == 0U || (vectorIndexId & (UINT64_C(1) << 63U)) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector-to-Node mapping requires a pinned snapshot and public Vector Index ID");
		}
		for (size_t probe = 0U; probe < RSS_VECTOR_NODE_MAPPING_MAX_ID_PROBES; ++probe) {
			const uint64_t mappingIndexId = candidateId(vectorIndexId, probe);
			if (!metadata.findIndexRootOptional(snapshot, mappingIndexId).has_value()) {
				return mappingIndexId;
			}
		}
		fail(FormatError::FERR_INVALID_STATE,
			 "relocatable Vector-to-Node mapping exhausted its bounded typed Index ID probe sequence");
	}

	BulkBTreeBuildResult VectorNodeMapping::build(BTreePageStore &pages, RelocatableMetadataStore &metadata,
													 uint64_t mappingIndexId,
													 const OrderedBTreeEntrySource &source,
													 const VectorNodeMappingBuildOptions &options,
													 LogSequenceNumber checkpointLsn,
													 const MetadataPublishOptions &publishOptions) {
		validateBuildInput(mappingIndexId, source, options, checkpointLsn);
		if (!pages.usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector-to-Node mapping requires one B+Tree and metadata store");
		}
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		if (metadata.findIndexRootOptional(snapshot, mappingIndexId).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector-to-Node mapping Index ID already owns an immutable B+Tree root");
		}
		snapshot.release();
		BulkBTreeBuildOptions tree = options.tree;
		// The pool belongs to this private mapping tree, never to a public Vector
		// index or a generic migration bucket.
		tree.pool_owner_id = mappingIndexId;
		return BulkBTreeBuilder::build(pages, metadata, mappingIndexId, source, tree, checkpointLsn,
									   publishOptions);
	}

	std::optional<uint64_t> VectorNodeMapping::findNode(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
															 RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
															 uint64_t vectorId) {
		validateMappingIndexId(mappingIndexId);
		if (!snapshot.isValid() || vectorId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector-to-Node mapping lookup requires a pinned snapshot and nonzero Vector ID");
		}
		const std::optional<uint64_t> nodeId = BTreeIndex(mappingIndexId, pages, metadata).find(snapshot, key(vectorId));
		if (nodeId.has_value() && *nodeId == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector-to-Node mapping B+Tree contains an invalid Node ID value");
		}
		return nodeId;
	}

	std::vector<std::byte> VectorNodeMapping::key(uint64_t vectorId) {
		return VectorSemanticCodec::vectorMappingKey(vectorId);
	}

} // namespace graph::storage
