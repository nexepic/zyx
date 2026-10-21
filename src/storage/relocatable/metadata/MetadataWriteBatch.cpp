/**
 * @file MetadataWriteBatch.cpp
 * @brief Ownership and publication of relocatable multi-root COW metadata batches.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include "graph/debug/PerfTrace.hpp"

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
                                           LogSequenceNumber checkpointLsn,
                                           std::shared_ptr<std::pmr::memory_resource> reservationMemory) :
        allocator_(allocator), current_(current), candidate_(current),
        reserved_data_slots_(utils::OwnedPmrAllocator<PhysicalSlotId>(reservationMemory)),
        reserved_data_slot_ranges_(utils::OwnedPmrAllocator<PhysicalSlotRange>(reservationMemory)),
        reserved_data_slot_index_(std::less<uint64_t>{}, utils::OwnedPmrAllocator<uint64_t>(reservationMemory)),
        reserved_data_slot_range_index_(std::less<uint64_t>{},
            utils::OwnedPmrAllocator<std::pair<const uint64_t, uint64_t>>(reservationMemory)) {
		SuperblockCodec::validate(current);
		if (!checkpointLsn.isValid() || checkpointLsn <= current.checkpoint_lsn ||
			current.generation.value() >= RSS_MAX_PHYSICAL_PAGE_GENERATION) {
			throwMetadataBatchError(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata batch needs an advanced representable generation and checkpoint LSN");
		}
		candidate_.generation = Generation(current.generation.value() + 1U);
		candidate_.checkpoint_lsn = checkpointLsn;
		initial_physical_file_size_ = allocator_.physicalFileSize();
		allocator_.beginUnpublishedBatch(current_);
		batch_context_active_ = true;
	}

	MetadataWriteBatch::~MetadataWriteBatch() {
		abandonUnpublishedPages();
		if (metadata_frame_reservation_active_) {
			allocator_.releaseMetadataFrames();
			metadata_frame_reservation_active_ = false;
		}
		if (batch_context_active_) {
			allocator_.endUnpublishedBatch(current_);
		}
	}

	void MetadataWriteBatch::reserveMetadataFrames(size_t count) {
		if (published_ || metadata_frame_reservation_active_ || count == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
							  "relocatable metadata batch cannot reserve frames in its current state");
		}
		allocator_.reserveMetadataFrames(count, std::span<const PhysicalSlotId>(reserved_data_slots_),
									 std::span<const PhysicalSlotRange>(reserved_data_slot_ranges_), this);
		metadata_frame_reservation_active_ = true;
	}

	size_t MetadataWriteBatch::ensureMetadataFrameCapacity(size_t count) {
		if (published_ || metadata_frame_reservation_active_ || count == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata batch cannot prepare route-frame capacity in its current state");
		}
		const std::vector<PhysicalSlotId> created = allocator_.ensureMetadataFrameCapacity(
				generation(), checkpointLsn(), count, std::span<const PhysicalSlotId>(reserved_data_slots_),
				std::span<const PhysicalSlotRange>(reserved_data_slot_ranges_), this);
		for (const PhysicalSlotId slot: created) {
			trackUnpublishedPool(slot);
		}
		return created.size();
	}

	bool MetadataWriteBatch::ownsAllocatedPage(PhysicalPageRef reference) const {
		return reference.isValid() && reference.generation == generation() &&
			   allocated_page_locations_.contains({reference.physical_slot.value(), reference.frame_index});
	}

	MetadataPageId MetadataWriteBatch::allocateMetadataPageId() {
		const MetadataPageId pageId = allocator_.allocateMetadataPageId();
		if (!pageId.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned an invalid Page ID");
		}
		return pageId;
	}

	void MetadataWriteBatch::registerMetadataPageRoute(MetadataPageId pageId, PhysicalPageRef reference) {
		if (!pageId.isValid() || !ownsAllocatedPage(reference)) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata route needs a stable ID and an owned current-generation frame");
		}
		metadata_page_routes_.insert_or_assign(pageId.value(), reference);
	}

	std::optional<PhysicalPageRef> MetadataWriteBatch::pendingMetadataPageRoute(MetadataPageId pageId) const {
		if (!pageId.isValid()) {
			return std::nullopt;
		}
		const auto iterator = metadata_page_routes_.find(pageId.value());
		return iterator == metadata_page_routes_.end() ? std::nullopt
																	 : std::optional<PhysicalPageRef>(iterator->second);
	}

	std::vector<MetadataPageRoute> MetadataWriteBatch::metadataPageRoutes() const {
		std::vector<MetadataPageRoute> routes;
		routes.reserve(metadata_page_routes_.size());
		for (const auto &[pageId, reference]: metadata_page_routes_) {
			routes.push_back({MetadataPageId(pageId), reference});
		}
		return routes;
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

	PageId MetadataWriteBatch::allocateLogicalPageIdRange(uint64_t count) {
		if (count == 0U || candidate_.next_logical_page_id == 0U ||
			count - 1U > std::numeric_limits<uint64_t>::max() - candidate_.next_logical_page_id) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable logical Page-ID range exceeds its durable namespace");
		}
		const PageId firstPageId(candidate_.next_logical_page_id);
		candidate_.next_logical_page_id =
				firstPageId.value() + count - 1U == std::numeric_limits<uint64_t>::max()
						? 0U
						: firstPageId.value() + count;
		return firstPageId;
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

	SegmentId MetadataWriteBatch::allocateLogicalSegmentIdRange(uint64_t count) {
		if (count == 0U || candidate_.next_logical_segment_id == 0U ||
			count - 1U > std::numeric_limits<uint64_t>::max() - candidate_.next_logical_segment_id) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable logical Segment ID range exceeds its durable namespace");
		}
		const SegmentId firstSegmentId(candidate_.next_logical_segment_id);
		candidate_.next_logical_segment_id =
				firstSegmentId.value() + count - 1U == std::numeric_limits<uint64_t>::max()
						? 0U
						: firstSegmentId.value() + count;
		return firstSegmentId;
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

	uint64_t MetadataWriteBatch::allocateLogicalNodeIdRange(uint64_t count) {
		if (count == 0U || candidate_.next_logical_node_id == 0U ||
			count - 1U > std::numeric_limits<uint64_t>::max() - candidate_.next_logical_node_id) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable logical Node ID range exceeds its durable namespace");
		}
		const uint64_t firstId = candidate_.next_logical_node_id;
		candidate_.next_logical_node_id =
				firstId + count - 1U == std::numeric_limits<uint64_t>::max() ? 0U : firstId + count;
		return firstId;
	}

	uint64_t MetadataWriteBatch::allocateLogicalEdgeIdRange(uint64_t count) {
		if (count == 0U || candidate_.next_logical_edge_id == 0U ||
			count - 1U > std::numeric_limits<uint64_t>::max() - candidate_.next_logical_edge_id) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"relocatable logical Edge ID range exceeds its durable namespace");
		}
		const uint64_t firstId = candidate_.next_logical_edge_id;
		candidate_.next_logical_edge_id =
				firstId + count - 1U == std::numeric_limits<uint64_t>::max() ? 0U : firstId + count;
		return firstId;
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

	LargeValueId MetadataWriteBatch::allocateLargeValueIdRange(uint64_t count) {
		if (count == 0U || candidate_.next_large_value_id == 0U ||
			count - 1U > std::numeric_limits<uint64_t>::max() - candidate_.next_large_value_id) {
			throwMetadataBatchError(FormatError::FERR_ARITHMETIC_OVERFLOW,
									"relocatable LargeValue ID range exceeds its durable namespace");
		}
		const LargeValueId firstId(candidate_.next_large_value_id);
		candidate_.next_large_value_id =
				firstId.value() + count - 1U == std::numeric_limits<uint64_t>::max() ? 0U : firstId.value() + count;
		return firstId;
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
				generation(), checkpointLsn(), std::span<const PhysicalSlotId>(reserved_data_slots_),
				std::span<const PhysicalSlotRange>(reserved_data_slot_ranges_), this);
		const PhysicalPageRef reference = allocation.page;
		if (!reference.isValid() || reference.generation != generation()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned an invalid physical reference");
		}
		const auto [location, inserted] =
				allocated_page_locations_.emplace(reference.physical_slot.value(), reference.frame_index);
		if (!inserted) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned a duplicate physical location");
		}
		try {
			allocated_pages_.push_back(reference);
		} catch (...) {
			allocated_page_locations_.erase(location);
			throw;
		}
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

	std::vector<PhysicalPageRef> MetadataWriteBatch::allocatePages(size_t count) {
		if (published_ || count == 0U) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								"relocatable metadata batch requires a nonempty unpublished page allocation");
		}
		allocated_pages_.reserve(allocated_pages_.size() + count);
		created_pool_slots_.reserve(created_pool_slots_.size() + count);
		const std::vector<MetadataPageAllocation> allocations = allocator_.allocatePages(
				generation(), checkpointLsn(), count, std::span<const PhysicalSlotId>(reserved_data_slots_),
				std::span<const PhysicalSlotRange>(reserved_data_slot_ranges_), this);
		if (allocations.size() != count) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								"relocatable metadata batch allocator returned an incomplete page allocation");
		}
		std::vector<PhysicalPageRef> pages;
		pages.reserve(count);
		for (const MetadataPageAllocation &allocation: allocations) {
			const PhysicalPageRef reference = allocation.page;
			if (!reference.isValid() || reference.generation != generation() ||
				!allocated_page_locations_.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
									"relocatable metadata batch allocator returned an invalid or duplicate physical reference");
			}
			allocated_pages_.push_back(reference);
			pages.push_back(reference);
			if (!allocation.created_pool_slot.has_value()) {
				continue;
			}
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
		return pages;
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
		const auto [iterator, inserted] = reserved_data_slot_index_.emplace(slot.value());
		if (inserted) {
			try {
				reserved_data_slots_.push_back(slot);
			} catch (...) {
				reserved_data_slot_index_.erase(iterator);
				throw;
			}
		}
	}

	void MetadataWriteBatch::reserveDataSlotRange(PhysicalSlotRange range) {
		if (!range.isValid()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch cannot reserve an invalid data-slot range");
		}
		if (std::ranges::any_of(created_pool_slots_, [&range](PhysicalSlotId slot) { return range.contains(slot); })) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch cannot reserve a metadata-pool slot as data");
		}
		const auto nextRange = reserved_data_slot_range_index_.lower_bound(range.first.value());
		const bool overlapsNext = nextRange != reserved_data_slot_range_index_.end() &&
							  nextRange->first <= range.last.value();
		const bool overlapsPrevious = nextRange != reserved_data_slot_range_index_.begin() &&
							  std::prev(nextRange)->second >= range.first.value();
		const auto firstReservedSlot = reserved_data_slot_index_.lower_bound(range.first.value());
		if (overlapsNext || overlapsPrevious ||
			(firstReservedSlot != reserved_data_slot_index_.end() && *firstReservedSlot <= range.last.value())) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch has overlapping data-slot reservations");
		}
		const auto [iterator, inserted] =
				reserved_data_slot_range_index_.emplace(range.first.value(), range.last.value());
		if (!inserted) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata batch has duplicate data-slot range reservations");
		}
		try {
			reserved_data_slot_ranges_.push_back(range);
		} catch (...) {
			reserved_data_slot_range_index_.erase(iterator);
			throw;
		}
	}

	std::optional<PhysicalSlotRange> MetadataWriteBatch::dataSlotRangeContaining(PhysicalSlotId slot) const noexcept {
		if (!slot.isValid()) {
			return std::nullopt;
		}
		const auto next = reserved_data_slot_range_index_.upper_bound(slot.value());
		if (next == reserved_data_slot_range_index_.begin()) {
			return std::nullopt;
		}
		const auto previous = std::prev(next);
		if (previous->second < slot.value()) {
			return std::nullopt;
		}
		return PhysicalSlotRange{PhysicalSlotId(previous->first), PhysicalSlotId(previous->second)};
	}

	bool MetadataWriteBatch::isDataSlotReserved(PhysicalSlotId slot) const noexcept {
		return slot.isValid() &&
			   (reserved_data_slot_index_.contains(slot.value()) || dataSlotRangeContaining(slot).has_value());
	}

	bool MetadataWriteBatch::isMetadataPoolSlot(PhysicalSlotId slot) const {
		return slot.isValid() && std::ranges::any_of(created_pool_slots_, [slot](PhysicalSlotId existing) {
			return existing == slot;
		});
	}

	void MetadataWriteBatch::setCatalogRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Catalog root");
		candidate_.catalog_root = root;
	}

	void MetadataWriteBatch::setPageDirectoryRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Page Directory root");
		if (candidate_.usesPageDirectoryBootstrap()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								"relocatable metadata batch must preserve the Page Directory bootstrap header");
		}
		candidate_.page_directory_root = root;
	}

	void MetadataWriteBatch::setPageDirectoryBootstrapRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid Page Directory bootstrap root");
		candidate_.page_directory_root = root;
		candidate_.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	}

	void MetadataWriteBatch::setPendingBootstrapDataRoot(PhysicalPageRef root) {
		validateOwnedRoot(root, "relocatable metadata batch has an invalid pending Page Directory root");
		if (!candidate_.usesPageDirectoryBootstrap()) {
			throwMetadataBatchError(FormatError::FERR_INVALID_STATE,
								"relocatable metadata batch has no bootstrap header for a deferred Page Directory root");
		}
		pending_bootstrap_data_root_ = root;
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

    void MetadataWriteBatch::enableHybridColumnMembership() {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "compact column membership requires the isolated native format");
        candidate_.flags |= RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP;
    }

    void MetadataWriteBatch::setHybridPropertyDescriptorRoot(PhysicalPageRef root) {
        if (root != PhysicalPageRef{})
            validateOwnedRoot(root, "hybrid property descriptor root is not owned by its metadata batch");
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED && root.isValid())
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "hybrid property format is disabled");
        candidate_.hybrid_property_descriptor_root = root;
        if (!root.isValid()) candidate_.flags &= ~RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP;
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
		if (!ownsAllocatedPage(root)) {
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
			debug::ScopedPerfTimer timer("native_graph.write.metadata.publication");
			bootstrap.publish(candidate_, failpoint);
			published_ = true;
		} catch (...) {
			published_ = candidateWasPublished(bootstrap, candidate_);
			if (metadata_frame_reservation_active_) {
				allocator_.releaseMetadataFrames();
				metadata_frame_reservation_active_ = false;
			}
			if (published_ && batch_context_active_) {
				allocator_.endUnpublishedBatch(current_);
				batch_context_active_ = false;
			}
			throw;
		}
		if (metadata_frame_reservation_active_) {
			allocator_.releaseMetadataFrames();
			metadata_frame_reservation_active_ = false;
		}
		if (batch_context_active_) {
			allocator_.endUnpublishedBatch(current_);
			batch_context_active_ = false;
		}
	}

	void MetadataWriteBatch::abandonUnpublishedPages() noexcept {
		if (!published_) {
			for (auto iterator = allocated_pages_.rbegin(); iterator != allocated_pages_.rend(); ++iterator) {
				allocator_.abandonPage(*iterator);
			}
			allocator_.abandonUnpublishedBatch(created_pool_slots_, reserved_data_slots_, reserved_data_slot_ranges_,
												 initial_physical_file_size_);
		}
		if (metadata_frame_reservation_active_) {
			allocator_.releaseMetadataFrames();
			metadata_frame_reservation_active_ = false;
		}
	}

} // namespace graph::storage
