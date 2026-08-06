/**
 * @file MetadataWriteBatch.cpp
 * @brief Ownership and publication of relocatable multi-root COW metadata batches.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace graph::storage {

	namespace {

		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;

		[[noreturn]] void throwMetadataBatchError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool sameSuperblock(const Superblock &left, const Superblock &right) { return left == right; }

		[[nodiscard]] bool candidateWasPublished(SuperblockFile &bootstrap, const Superblock &candidate) noexcept {
			try {
				return sameSuperblock(bootstrap.recover(), candidate);
			} catch (...) {
				// A failed recovery cannot prove that any written root is unreachable.
				return true;
			}
		}

	} // namespace

	MetadataWriteBatch::MetadataWriteBatch(MetadataPageAllocator &allocator, const Superblock &current,
										   LogSequenceNumber checkpointLsn) :
		allocator_(allocator), current_(current), candidate_(current) {
		static_cast<void>(SuperblockCodec::encode(current));
		if (!checkpointLsn.isValid() || checkpointLsn <= current.checkpoint_lsn ||
			current.generation.value() >= RSS_MAX_PHYSICAL_PAGE_GENERATION) {
			throwMetadataBatchError(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata batch needs an advanced representable generation and checkpoint LSN");
		}
		candidate_.generation = Generation(current.generation.value() + 1U);
		candidate_.checkpoint_lsn = checkpointLsn;
		initial_physical_file_size_ = allocator_.physicalFileSize();
	}

	MetadataWriteBatch::~MetadataWriteBatch() { abandonUnpublishedPages(); }

	PageId MetadataWriteBatch::allocatePageId() {
		const PageId pageId = allocator_.allocatePageId();
		if (!pageId.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned an invalid Page ID");
		}
		return pageId;
	}

	PageId MetadataWriteBatch::allocateLogicalPageId() {
		if (candidate_.next_logical_page_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Page ID space is exhausted");
		}
		const PageId pageId(candidate_.next_logical_page_id);
		if (candidate_.next_logical_page_id == std::numeric_limits<uint64_t>::max()) {
			candidate_.next_logical_page_id = 0U;
		} else {
			++candidate_.next_logical_page_id;
		}
		return pageId;
	}

	SegmentId MetadataWriteBatch::allocateLogicalSegmentId() {
		if (candidate_.next_logical_segment_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Segment ID space is exhausted");
		}
		const SegmentId segmentId(candidate_.next_logical_segment_id);
		if (candidate_.next_logical_segment_id == std::numeric_limits<uint64_t>::max()) {
			candidate_.next_logical_segment_id = 0U;
		} else {
			++candidate_.next_logical_segment_id;
		}
		return segmentId;
	}

	uint64_t MetadataWriteBatch::allocateLogicalNodeId() {
		if (candidate_.next_logical_node_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Node ID space is exhausted");
		}
		const uint64_t nodeId = candidate_.next_logical_node_id;
		candidate_.next_logical_node_id = nodeId == std::numeric_limits<uint64_t>::max() ? 0U : nodeId + 1U;
		return nodeId;
	}

	uint64_t MetadataWriteBatch::allocateLogicalEdgeId() {
		if (candidate_.next_logical_edge_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Edge ID space is exhausted");
		}
		const uint64_t edgeId = candidate_.next_logical_edge_id;
		candidate_.next_logical_edge_id = edgeId == std::numeric_limits<uint64_t>::max() ? 0U : edgeId + 1U;
		return edgeId;
	}

	uint64_t MetadataWriteBatch::allocateLogicalVectorId() {
		if (candidate_.next_logical_vector_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Vector ID space is exhausted");
		}
		const uint64_t vectorId = candidate_.next_logical_vector_id;
		candidate_.next_logical_vector_id = vectorId == std::numeric_limits<uint64_t>::max() ? 0U : vectorId + 1U;
		return vectorId;
	}

	LargeValueId MetadataWriteBatch::allocateLargeValueId() {
		if (candidate_.next_large_value_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable LargeValue ID space is exhausted");
		}
		const LargeValueId valueId(candidate_.next_large_value_id);
		candidate_.next_large_value_id =
				valueId.value() == std::numeric_limits<uint64_t>::max() ? 0U : valueId.value() + 1U;
		return valueId;
	}

	uint32_t MetadataWriteBatch::allocatePropertyKeyToken() {
		if (candidate_.next_property_key_token == 0U ||
			candidate_.next_property_key_token > std::numeric_limits<uint32_t>::max()) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable Property-key Token space is exhausted");
		}
		const uint32_t token = static_cast<uint32_t>(candidate_.next_property_key_token);
		candidate_.next_property_key_token =
				token == std::numeric_limits<uint32_t>::max() ? 0U : static_cast<uint64_t>(token) + 1U;
		return token;
	}

	uint64_t MetadataWriteBatch::allocateSchemaTokenId() {
		if (candidate_.next_schema_token_id == 0U) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable schema Token ID space is exhausted");
		}
		const uint64_t tokenId = candidate_.next_schema_token_id;
		candidate_.next_schema_token_id = tokenId == std::numeric_limits<uint64_t>::max() ? 0U : tokenId + 1U;
		return tokenId;
	}

	void MetadataWriteBatch::observeLogicalPageId(PageId pageId) {
		if (!pageId.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid logical Page ID");
		}
		if (candidate_.next_logical_page_id == 0U || pageId.value() < candidate_.next_logical_page_id) {
			return;
		}
		if (pageId.value() == std::numeric_limits<uint64_t>::max()) {
			candidate_.next_logical_page_id = 0U;
			return;
		}
		candidate_.next_logical_page_id = pageId.value() + 1U;
	}

	void MetadataWriteBatch::observeLogicalSegmentId(SegmentId segmentId) {
		if (!segmentId.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid logical Segment ID");
		}
		if (candidate_.next_logical_segment_id == 0U || segmentId.value() < candidate_.next_logical_segment_id) {
			return;
		}
		if (segmentId.value() == std::numeric_limits<uint64_t>::max()) {
			candidate_.next_logical_segment_id = 0U;
			return;
		}
		candidate_.next_logical_segment_id = segmentId.value() + 1U;
	}

	void MetadataWriteBatch::observeLogicalNodeId(uint64_t nodeId) {
		if (nodeId == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid logical Node ID");
		}
		if (candidate_.next_logical_node_id == 0U || nodeId < candidate_.next_logical_node_id) {
			return;
		}
		candidate_.next_logical_node_id = nodeId == std::numeric_limits<uint64_t>::max() ? 0U : nodeId + 1U;
	}

	void MetadataWriteBatch::observeLogicalEdgeId(uint64_t edgeId) {
		if (edgeId == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid logical Edge ID");
		}
		if (candidate_.next_logical_edge_id == 0U || edgeId < candidate_.next_logical_edge_id) {
			return;
		}
		candidate_.next_logical_edge_id = edgeId == std::numeric_limits<uint64_t>::max() ? 0U : edgeId + 1U;
	}

	void MetadataWriteBatch::observeLogicalVectorId(uint64_t vectorId) {
		if (vectorId == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid logical Vector ID");
		}
		if (candidate_.next_logical_vector_id == 0U || vectorId < candidate_.next_logical_vector_id) {
			return;
		}
		candidate_.next_logical_vector_id = vectorId == std::numeric_limits<uint64_t>::max() ? 0U : vectorId + 1U;
	}

	void MetadataWriteBatch::observeLargeValueId(LargeValueId valueId) {
		if (!valueId.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid LargeValue ID");
		}
		if (candidate_.next_large_value_id == 0U || valueId.value() < candidate_.next_large_value_id) {
			return;
		}
		candidate_.next_large_value_id =
				valueId.value() == std::numeric_limits<uint64_t>::max() ? 0U : valueId.value() + 1U;
	}

	void MetadataWriteBatch::observePropertyKeyToken(uint32_t token) {
		if (token == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid Property-key Token");
		}
		if (candidate_.next_property_key_token == 0U || token < candidate_.next_property_key_token) {
			return;
		}
		candidate_.next_property_key_token =
				token == std::numeric_limits<uint32_t>::max() ? 0U : static_cast<uint64_t>(token) + 1U;
	}

	void MetadataWriteBatch::observeSchemaTokenId(uint64_t tokenId) {
		if (tokenId == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch cannot observe an invalid schema Token ID");
		}
		if (candidate_.next_schema_token_id == 0U || tokenId < candidate_.next_schema_token_id) {
			return;
		}
		candidate_.next_schema_token_id = tokenId == std::numeric_limits<uint64_t>::max() ? 0U : tokenId + 1U;
	}

	PhysicalPageRef MetadataWriteBatch::allocatePage() {
		const MetadataPageAllocation allocation = allocator_.allocatePage(
				generation(), checkpointLsn(), std::span<const PhysicalSlotId>(reserved_data_slots_));
		const PhysicalPageRef reference = allocation.page;
		if (!reference.isValid() || reference.generation != generation()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned an invalid physical reference");
		}
		for (const PhysicalPageRef &allocated: allocated_pages_) {
			if (allocated.physical_slot == reference.physical_slot && allocated.frame_index == reference.frame_index) {
				throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
										"relocatable metadata batch allocator returned a duplicate physical location");
			}
		}
		allocated_pages_.push_back(reference);
		if (allocation.created_pool_slot.has_value()) {
			const PhysicalSlotId poolSlot = *allocation.created_pool_slot;
			const bool duplicatePool = std::ranges::any_of(
					created_pool_slots_, [poolSlot](PhysicalSlotId existing) { return existing == poolSlot; });
			if (!poolSlot.isValid() || poolSlot != reference.physical_slot || duplicatePool ||
				isDataSlotReserved(poolSlot)) {
				throwMetadataBatchError(
						FormatError::FERR_INVALID_STATE,
						"relocatable metadata batch allocator returned an invalid new pool ownership record");
			}
			created_pool_slots_.push_back(poolSlot);
		}
		return reference;
	}

	PhysicalSlotId MetadataWriteBatch::createdPoolSlot(size_t index) const {
		if (index >= created_pool_slots_.size()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch pool ownership index is invalid");
		}
		return created_pool_slots_[index];
	}

	void MetadataWriteBatch::trackUnpublishedPool(PhysicalSlotId slot) {
		if (!slot.isValid() || std::ranges::any_of(created_pool_slots_,
										  [slot](PhysicalSlotId existing) { return existing == slot; }) ||
			isDataSlotReserved(slot)) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch cannot track an invalid or duplicate unpublished pool");
		}
		created_pool_slots_.push_back(slot);
	}

	void MetadataWriteBatch::reserveDataSlot(PhysicalSlotId slot) {
		if (!slot.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch cannot reserve an invalid data slot");
		}
		if (std::ranges::any_of(created_pool_slots_, [slot](PhysicalSlotId existing) { return existing == slot; })) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch cannot reserve a metadata-pool slot as data");
		}
		const bool duplicate =
				std::ranges::any_of(reserved_data_slots_, [slot](PhysicalSlotId existing) { return existing == slot; });
		if (!duplicate) {
			reserved_data_slots_.push_back(slot);
		}
	}

	bool MetadataWriteBatch::isDataSlotReserved(PhysicalSlotId slot) const {
		return slot.isValid() &&
			   std::ranges::any_of(reserved_data_slots_, [slot](PhysicalSlotId existing) { return existing == slot; });
	}

	void MetadataWriteBatch::setCatalogRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Catalog root");
		candidate_.catalog_root = root;
	}

	void MetadataWriteBatch::setPageDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Page Directory root");
		candidate_.page_directory_root = root;
	}

	void MetadataWriteBatch::setExtentDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Extent Directory root");
		candidate_.extent_directory_root = root;
	}

	void MetadataWriteBatch::setAllocatorRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid free-space root");
		candidate_.allocator_root = root;
	}

	void MetadataWriteBatch::setNodeEntityDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Node Entity Directory root");
		candidate_.node_entity_directory_root = root;
	}

	void MetadataWriteBatch::setEdgeEntityDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Edge Entity Directory root");
		candidate_.edge_entity_directory_root = root;
	}

	void MetadataWriteBatch::setEntityPropertyDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Entity Property Directory root");
		candidate_.entity_property_directory_root = root;
	}

	void MetadataWriteBatch::setIndexRootDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Index Root Directory root");
		candidate_.index_root_directory_root = root;
	}

	void MetadataWriteBatch::setVectorLocationDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Vector Location Directory root");
		candidate_.vector_location_directory_root = root;
	}

	void MetadataWriteBatch::setVectorManifestDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Vector Manifest Directory root");
		candidate_.vector_manifest_directory_root = root;
	}

	void MetadataWriteBatch::setLargeValueManifestDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid LargeValue Manifest Directory root");
		candidate_.large_value_manifest_directory_root = root;
	}

	void MetadataWriteBatch::setStateDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid State Directory root");
		candidate_.state_directory_root = root;
	}

	void MetadataWriteBatch::setRetireQueueRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Retire Queue root");
		candidate_.retire_queue_root = root;
	}

	void MetadataWriteBatch::setMetadataEmergencyReserve(MetadataEmergencyReserveDescriptor reserve) {
		const uint8_t invalid = static_cast<uint8_t>(!reserve.isValid()) |
								static_cast<uint8_t>(reserve.generation != generation()) |
								static_cast<uint8_t>(reserve.committed_lsn != checkpointLsn());
		if (invalid != 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_HEADER,
									"relocatable metadata batch has an invalid emergency reserve descriptor");
		}
		candidate_.metadata_emergency_reserve = reserve;
	}

	void MetadataWriteBatch::validateOwnedRoot(PhysicalPageRef root, const char *message) const {
		const bool owned = std::ranges::any_of(allocated_pages_, [&](const PhysicalPageRef &allocated) {
			return allocated.physical_slot == root.physical_slot && allocated.frame_index == root.frame_index;
		});
		const uint8_t invalid = static_cast<uint8_t>(!root.isValid()) |
								static_cast<uint8_t>(root.generation != generation()) | static_cast<uint8_t>(!owned);
		if (invalid != 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE, message);
		}
	}

	void MetadataWriteBatch::publish(SuperblockFile &bootstrap, BootstrapPublishFailpoint failpoint) {
		if (published_) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch was already published");
		}
		if (!sameSuperblock(bootstrap.recover(), current_)) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch uses a stale Superblock generation");
		}
		try {
			bootstrap.publish(candidate_, failpoint);
			published_ = true;
		} catch (...) {
			published_ = candidateWasPublished(bootstrap, candidate_);
			throw;
		}
	}

	void MetadataWriteBatch::abandonUnpublishedPages() noexcept {
		if (!published_) {
			for (auto iterator = allocated_pages_.rbegin(); iterator != allocated_pages_.rend(); ++iterator) {
				allocator_.abandonPage(*iterator);
			}
			allocator_.abandonUnpublishedBatch(created_pool_slots_, reserved_data_slots_, initial_physical_file_size_);
		}
	}

} // namespace graph::storage
