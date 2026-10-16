/**
 * @file VectorDeltaMapping.cpp
 * @brief Typed VectorId overlay mapping for mutable Vector derived state.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"

#include <limits>
#include <memory>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		constexpr uint64_t RSS_VECTOR_DELTA_MAPPING_ID_SUFFIX_MASK = UINT64_C(0x0000FFFFFFFFFFFF);
		constexpr uint64_t RSS_VECTOR_DELTA_MAPPING_ID_SALT = UINT64_C(0xF1357AEA2E62A9C5);
		constexpr size_t RSS_VECTOR_DELTA_MAPPING_MAX_ID_PROBES = 256U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint64_t mixId(uint64_t value) {
			value += UINT64_C(0x9E3779B97F4A7C15);
			value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
			value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
			return value ^ (value >> 31U);
		}

		[[nodiscard]] uint64_t candidateId(uint64_t vectorIndexId, size_t probe) {
			return RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX |
				   (mixId(vectorIndexId ^ RSS_VECTOR_DELTA_MAPPING_ID_SALT ^ static_cast<uint64_t>(probe)) &
					RSS_VECTOR_DELTA_MAPPING_ID_SUFFIX_MASK);
		}

		[[nodiscard]] uint64_t decodeVectorId(std::span<const std::byte> key) {
			if (key.size() != sizeof(uint64_t) + sizeof(uint8_t)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector delta mapping key does not have the canonical Vector-ID and operation width");
			}
			uint64_t value = 0U;
			for (size_t ordinal = 0U; ordinal < sizeof(uint64_t); ++ordinal) {
				value = (value << 8U) | static_cast<uint64_t>(std::to_integer<uint8_t>(key[ordinal]));
			}
			return value;
		}

		[[nodiscard]] VectorDeltaOperation decodeKeyOperation(std::span<const std::byte> key) {
			static_cast<void>(decodeVectorId(key));
			return VectorDeltaMapping::decodeOperation(std::to_integer<uint8_t>(key.back()));
		}

		void validateBuildInput(uint64_t mappingIndexId, std::span<const VectorDeltaEntry> entries,
								const VectorDeltaMappingBuildOptions &options, LogSequenceNumber checkpointLsn) {
			VectorDeltaMapping::validateMappingIndexId(mappingIndexId);
			const uint8_t invalid = static_cast<uint8_t>(entries.empty()) |
									static_cast<uint8_t>(options.maximum_entries == 0U) |
									static_cast<uint8_t>(entries.size() > options.maximum_entries) |
									static_cast<uint8_t>(options.tree.maximum_entries != options.maximum_entries) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector delta mapping requires bounded nonempty entries and a checkpoint LSN");
			}
			uint64_t previousVectorId = 0U;
			for (const VectorDeltaEntry &entry: entries) {
				if (entry.vector_id == 0U || entry.node_id == 0U || entry.vector_id <= previousVectorId) {
					fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector delta mapping entries must have "
														   "strictly ordered nonzero Vector and Node IDs");
				}
				static_cast<void>(VectorDeltaMapping::encodeOperation(entry.operation));
				previousVectorId = entry.vector_id;
			}
		}

	} // namespace

	void VectorDeltaMapping::validateMappingIndexId(uint64_t mappingIndexId) {
		if (!isMappingIndexId(mappingIndexId)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector delta mapping requires a metadata-owned typed derived Index ID");
		}
	}

	uint64_t VectorDeltaMapping::selectMappingIndexId(const MetadataSnapshotPin &snapshot,
													  RelocatableMetadataStore &metadata, uint64_t vectorIndexId) {
		if (!snapshot.isValid() || vectorIndexId == 0U || (vectorIndexId & (UINT64_C(1) << 63U)) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector delta mapping requires a pinned snapshot and public Vector Index ID");
		}
		for (size_t probe = 0U; probe < RSS_VECTOR_DELTA_MAPPING_MAX_ID_PROBES; ++probe) {
			const uint64_t mappingIndexId = candidateId(vectorIndexId, probe);
			if (!metadata.findIndexRootOptional(snapshot, mappingIndexId).has_value()) {
				return mappingIndexId;
			}
		}
		fail(FormatError::FERR_INVALID_STATE,
			 "relocatable Vector delta mapping exhausted its bounded typed Index ID probe sequence");
	}

	BulkBTreeBuildResult VectorDeltaMapping::build(BTreePageStore &pages, RelocatableMetadataStore &metadata,
												   uint64_t mappingIndexId, std::span<const VectorDeltaEntry> entries,
												   const VectorDeltaMappingBuildOptions &options,
												   LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &publishOptions) {
		validateBuildInput(mappingIndexId, entries, options, checkpointLsn);
		if (!pages.usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta mapping requires one B+Tree and metadata store");
		}
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		if (metadata.findIndexRootOptional(snapshot, mappingIndexId).has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta mapping Index ID already owns an immutable B+Tree root");
		}
		snapshot.release();
		utils::OwnedStdVector<BTreePageEntry> treeEntries;
		treeEntries.reserve(entries.size());
		for (const VectorDeltaEntry &entry: entries) {
			treeEntries.push_back({key(entry.vector_id, entry.operation), entry.node_id});
		}
		const auto sharedEntries = std::make_shared<const utils::OwnedStdVector<BTreePageEntry>>(std::move(treeEntries));
		const OrderedBTreeEntrySource source = [sharedEntries] {
			auto cursor = std::make_shared<size_t>(0U);
			return [sharedEntries, cursor]() mutable -> std::optional<BTreePageEntry> {
				if (*cursor == sharedEntries->size()) {
					return std::nullopt;
				}
				return (*sharedEntries)[(*cursor)++];
			};
		};
		BulkBTreeBuildOptions tree = options.tree;
		tree.pool_owner_id = mappingIndexId;
		return BulkBTreeBuilder::build(pages, metadata, mappingIndexId, source, tree, checkpointLsn, publishOptions);
	}

	std::optional<VectorDeltaOperation> VectorDeltaMapping::findOperation(const MetadataSnapshotPin &snapshot,
																		  BTreePageStore &pages,
																		  RelocatableMetadataStore &metadata,
																		  uint64_t mappingIndexId, uint64_t vectorId) {
		validateMappingIndexId(mappingIndexId);
		if (!snapshot.isValid() || vectorId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector delta mapping lookup requires a pinned snapshot and nonzero Vector ID");
		}
		BTreeIndex index(mappingIndexId, pages, metadata);
		const std::optional<uint64_t> insertNode =
				index.find(snapshot, key(vectorId, VectorDeltaOperation::VDO_INSERT));
		const std::optional<uint64_t> eraseNode = index.find(snapshot, key(vectorId, VectorDeltaOperation::VDO_ERASE));
		if (insertNode.has_value() && eraseNode.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta mapping has contradictory operations for one Vector ID");
		}
		if (insertNode.has_value()) {
			if (*insertNode == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector delta mapping has a zero Node ID in an insert entry");
			}
			return VectorDeltaOperation::VDO_INSERT;
		}
		if (eraseNode.has_value()) {
			if (*eraseNode == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector delta mapping has a zero Node ID in an erase entry");
			}
			return VectorDeltaOperation::VDO_ERASE;
		}
		return std::nullopt;
	}

    std::vector<VectorDeltaEntry> VectorDeltaMapping::scan(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
            RelocatableMetadataStore &metadata, uint64_t mappingIndexId, size_t maximumEntries,
            const CancellationCheckpoint *cancellation) {
        auto owned = scanOwned(snapshot, pages, metadata, mappingIndexId, maximumEntries, cancellation);
        // Legacy owning API: the caller assumes responsibility at this boundary.
        return std::move(static_cast<std::vector<VectorDeltaEntry> &>(owned));
    }

	utils::OwnedStdVector<VectorDeltaEntry> VectorDeltaMapping::scanOwned(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
																   RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
																   size_t maximumEntries, const CancellationCheckpoint *cancellation) {
		validateMappingIndexId(mappingIndexId);
		if (!snapshot.isValid() || maximumEntries == 0U || maximumEntries == std::numeric_limits<size_t>::max()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector delta mapping scan requires a bounded pinned snapshot");
		}
		const utils::OwnedStdVector<BTreePageEntry> entries =
				BTreeIndex(mappingIndexId, pages, metadata)
						.scan(snapshot, std::nullopt, maximumEntries + 1U, BufferPoolReadHint::BPRH_SEQUENTIAL, cancellation);
		if (entries.size() > maximumEntries) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta mapping exceeds its configured bounded scan cardinality");
		}
		utils::OwnedStdVector<VectorDeltaEntry> result;
		result.reserve(entries.size());
		uint64_t previousVectorId = 0U;
		for (const BTreePageEntry &entry: entries) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
			const VectorDeltaEntry decoded = decodeEntry(entry);
			const uint64_t vectorId = decoded.vector_id;
			if (vectorId == 0U || vectorId <= previousVectorId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector delta mapping contains unordered or invalid Vector IDs");
			}
			result.push_back(decoded);
			previousVectorId = vectorId;
		}
		return result;
	}

	void VectorDeltaMapping::validateCurrentRawMembership(const VectorDeltaEntry &entry,
														 bool hasCurrentRawLocation) {
		if (entry.vector_id == 0U || entry.node_id == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta membership requires nonzero Vector and Node IDs");
		}
		if (entry.operation == VectorDeltaOperation::VDO_INSERT) {
			if (!hasCurrentRawLocation) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector delta insert has no current Raw Vector location");
			}
			return;
		}
		if (entry.operation == VectorDeltaOperation::VDO_ERASE) {
			if (hasCurrentRawLocation) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable Vector delta erase still has a current Raw Vector location");
			}
			return;
		}
		fail(FormatError::FERR_INVALID_STATE, "relocatable Vector delta membership has an invalid operation");
	}

	std::vector<std::byte> VectorDeltaMapping::key(uint64_t vectorId, VectorDeltaOperation operation) {
		if (vectorId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector delta mapping requires a nonzero Vector ID key");
		}
		std::vector<std::byte> result = VectorSemanticCodec::vectorMappingKey(vectorId);
		result.push_back(static_cast<std::byte>(encodeOperation(operation)));
		return result;
	}

	VectorDeltaEntry VectorDeltaMapping::decodeEntry(const BTreePageEntry &entry) {
		const uint64_t vectorId = decodeVectorId(entry.key);
		const VectorDeltaOperation operation = decodeKeyOperation(entry.key);
		if (vectorId == 0U || entry.value_or_child == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable Vector delta mapping entry requires nonzero Vector and Node IDs");
		}
		return {vectorId, entry.value_or_child, operation};
	}

	uint64_t VectorDeltaMapping::encodeOperation(VectorDeltaOperation operation) {
		if (operation != VectorDeltaOperation::VDO_INSERT && operation != VectorDeltaOperation::VDO_ERASE) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector delta mapping has an invalid operation");
		}
		return static_cast<uint64_t>(operation);
	}

	VectorDeltaOperation VectorDeltaMapping::decodeOperation(uint64_t value) {
		if (value == encodeOperation(VectorDeltaOperation::VDO_INSERT)) {
			return VectorDeltaOperation::VDO_INSERT;
		}
		if (value == encodeOperation(VectorDeltaOperation::VDO_ERASE)) {
			return VectorDeltaOperation::VDO_ERASE;
		}
		fail(FormatError::FERR_INVALID_STATE, "relocatable Vector delta mapping has an invalid stored operation");
	}

} // namespace graph::storage
