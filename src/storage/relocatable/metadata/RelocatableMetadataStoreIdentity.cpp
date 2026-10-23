/**
 * @file RelocatableMetadataStoreIdentity.cpp
 * @brief Durable logical identity allocation and retire-queue publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

#include <vector>

namespace graph::storage {

	PageId RelocatableMetadataStore::allocateLogicalPageId(LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const PageId pageId = batch.allocateLogicalPageId();
		publishBatch(batch, writer, options);
		return pageId;
	}

	std::vector<PageId> RelocatableMetadataStore::allocateLogicalPageIds(size_t count, LogSequenceNumber checkpointLsn,
																					 const MetadataPublishOptions &options) {
		if (count == 0U || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable logical Page ID reservation requires a positive count and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<PageId> pageIds;
		pageIds.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			pageIds.push_back(batch.allocateLogicalPageId());
		}
		publishBatch(batch, writer, options);
		return pageIds;
	}

	std::vector<SegmentId> RelocatableMetadataStore::allocateLogicalSegmentIds(size_t count,
																					   LogSequenceNumber checkpointLsn,
																					   const MetadataPublishOptions &options) {
		if (count == 0U || !checkpointLsn.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable logical Segment ID reservation requires a positive count and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
        // Namespace-only reservations do not alter graph/native ownership.
        // Native scratch callers retain the existing bounded64-fragment batch.
        if (current.hybrid_property_descriptor_root.isValid() && count > 64U)
            throw FormatException(FormatError::FERR_INVALID_STATE, "native logical Segment reservation exceeds64IDs");
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        if (current.hybrid_property_descriptor_root.isValid()) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
		std::vector<SegmentId> segmentIds;
		segmentIds.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			segmentIds.push_back(batch.allocateLogicalSegmentId());
		}
		publishBatch(batch, writer, options);
		return segmentIds;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalNodeId(LogSequenceNumber checkpointLsn,
																					 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t nodeId = batch.allocateLogicalNodeId();
		publishBatch(batch, writer, options);
		return nodeId;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalEdgeId(LogSequenceNumber checkpointLsn,
																					 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t edgeId = batch.allocateLogicalEdgeId();
		publishBatch(batch, writer, options);
		return edgeId;
	}

	uint64_t RelocatableMetadataStore::allocateLogicalVectorId(LogSequenceNumber checkpointLsn,
																					   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t vectorId = batch.allocateLogicalVectorId();
		publishBatch(batch, writer, options);
		return vectorId;
	}

	uint32_t RelocatableMetadataStore::allocatePropertyKeyToken(LogSequenceNumber checkpointLsn,
																						const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint32_t token = batch.allocatePropertyKeyToken();
		publishBatch(batch, writer, options);
		return token;
	}

	void RelocatableMetadataStore::advancePropertyKeyTokenFloor(uint32_t token, LogSequenceNumber checkpointLsn,
																						const MetadataPublishOptions &options) {
		if (token == 0U || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Property-key Token floor requires a nonzero Token and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.next_property_key_token == 0U || current.next_property_key_token > token) {
			return;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.observePropertyKeyToken(token);
		publishBatch(batch, writer, options);
	}

	uint64_t RelocatableMetadataStore::allocateSchemaTokenId(LogSequenceNumber checkpointLsn,
																					 const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const uint64_t tokenId = batch.allocateSchemaTokenId();
		publishBatch(batch, writer, options);
		return tokenId;
	}

	void RelocatableMetadataStore::advanceSchemaTokenIdFloor(uint64_t tokenId, LogSequenceNumber checkpointLsn,
																					 const MetadataPublishOptions &options) {
		if (tokenId == 0U || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable schema Token floor requires a nonzero Token ID and checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.next_schema_token_id == 0U || current.next_schema_token_id > tokenId) {
			return;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		batch.observeSchemaTokenId(tokenId);
		publishBatch(batch, writer, options);
	}

	RetiredPhysicalSlotReclamation
	RelocatableMetadataStore::reclaimRetiredPhysicalSlots(LogSequenceNumber checkpointLsn,
																					  const MetadataPublishOptions &options) {
		if (!checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Retire Queue reclamation requires a checkpoint LSN");
		}
		// This entry consumes only already durable retirement records. The
		// reclaimer authenticates Catalog/allocator ownership and every active
		// snapshot; it never discovers new field retirement or changes hybrid
		// authority. Keep other unadapted writes behind their existing guard.
		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
		const std::vector<RetireQueueEntry> reclaimable = retire_queue_reclaimer_.collectReclaimable(current, writer);
		if (reclaimable.empty()) {
			return {current, 0U};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		retire_queue_reclaimer_.prepareReclamation(batch, reclaimable);
		// The authenticated queue is the complete scope of this physical-only
		// publication; no caller-supplied field or descriptor images are admitted.
		batch.hybrid_property_publication_ = true;
		publishBatch(batch, writer, options);
		return {batch.candidate(), static_cast<uint64_t>(reclaimable.size())};
	}

} // namespace graph::storage
