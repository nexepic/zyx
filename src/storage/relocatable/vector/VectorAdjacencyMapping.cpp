/**
 * @file VectorAdjacencyMapping.cpp
 * @brief Derived adjacency mapping identity helpers.
 */

#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"

#include <memory>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	namespace {

		void validateBuildInput(uint64_t mappingIndexId, std::span<const VectorAdjacencyPageMapping> mappings,
								const VectorAdjacencyMappingBuildOptions &options, LogSequenceNumber checkpointLsn) {
			VectorAdjacencyMapping::validateMappingIndexId(mappingIndexId);
			const uint8_t invalid = static_cast<uint8_t>(mappings.empty()) |
								static_cast<uint8_t>(mappings.size() > options.maximum_entries) |
								static_cast<uint8_t>(options.maximum_entries == 0U) |
								static_cast<uint8_t>(options.tree.maximum_entries != options.maximum_entries) |
								static_cast<uint8_t>(options.tree.pool_owner_id != mappingIndexId) |
								static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable adjacency mapping requires bounded nonempty entries and a checkpoint LSN");
			}
			uint64_t previousId = 0U;
			for (const VectorAdjacencyPageMapping &mapping: mappings) {
				if (mapping.vector_id == 0U || mapping.vector_id <= previousId || !mapping.page_id.isValid()) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "relocatable adjacency mapping entries are not strictly ordered Vector IDs with valid Page IDs");
				}
				previousId = mapping.vector_id;
			}
		}

	} // namespace

	void VectorAdjacencyMapping::validateMappingIndexId(uint64_t mappingIndexId) {
		if (!isMappingIndexId(mappingIndexId)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency mapping requires a metadata-owned typed derived Index ID");
		}
	}

	BulkBTreeBuildResult VectorAdjacencyMapping::build(BTreePageStore &pages, RelocatableMetadataStore &metadata,
													   uint64_t mappingIndexId,
													   std::span<const VectorAdjacencyPageMapping> mappings,
													   const VectorAdjacencyMappingBuildOptions &options,
													   LogSequenceNumber checkpointLsn,
													   const MetadataPublishOptions &publishOptions) {
		validateBuildInput(mappingIndexId, mappings, options, checkpointLsn);
		if (!pages.usesMetadataStore(metadata)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency mapping requires one B+Tree and metadata store");
		}
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		if (metadata.findIndexRootOptional(snapshot, mappingIndexId).has_value()) {
			snapshot.release();
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency mapping Index ID already owns an immutable B+Tree root");
		}
		snapshot.release();
		std::vector<BTreePageEntry> entries;
		entries.reserve(mappings.size());
		for (const VectorAdjacencyPageMapping &mapping: mappings) {
			entries.push_back({key(mapping.vector_id), mapping.page_id.value()});
		}
		const auto sharedEntries = std::make_shared<const std::vector<BTreePageEntry>>(std::move(entries));
		const OrderedBTreeEntrySource source = [sharedEntries] {
			auto cursor = std::make_shared<size_t>(0U);
			return [sharedEntries, cursor]() mutable -> std::optional<BTreePageEntry> {
				if (*cursor == sharedEntries->size()) {
					return std::nullopt;
				}
				return (*sharedEntries)[(*cursor)++];
			};
		};
		return BulkBTreeBuilder::build(pages, metadata, mappingIndexId, source, options.tree, checkpointLsn,
									  publishOptions);
	}

	std::optional<PageId> VectorAdjacencyMapping::findPage(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
														   RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
														   uint64_t vectorId) {
		validateMappingIndexId(mappingIndexId);
		if (vectorId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency mapping lookup requires a nonzero Vector ID");
		}
		const std::optional<uint64_t> pageId = BTreeIndex(mappingIndexId, pages, metadata).find(snapshot, key(vectorId));
		if (!pageId.has_value()) {
			return std::nullopt;
		}
		const PageId result(*pageId);
		if (!result.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable adjacency mapping B+Tree contains an invalid Page ID value");
		}
		return result;
	}

	std::vector<std::byte> VectorAdjacencyMapping::key(uint64_t vectorId) {
		if (vectorId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency mapping key requires a nonzero Vector ID");
		}
		return VectorSemanticCodec::vectorMappingKey(vectorId);
	}

} // namespace graph::storage
