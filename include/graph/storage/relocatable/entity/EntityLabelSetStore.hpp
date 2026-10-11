/**
 * @file EntityLabelSetStore.hpp
 * @brief Atomic durable Node-label storage on native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/EntityPageAuxiliaryMutation.hpp"

namespace graph::storage {

	// The caller supplies Page IDs because logical Page-ID allocation belongs to
	// the enclosing graph transaction. The store validates the current Catalog
	// identity before either fallback can become reachable.
	struct EntityLabelSetAppendRequest {
		SegmentId segment_id{};
		PageId page_id{};
	};

	// A graph mutation may need to grow the shared Entity-page pool when every
	// candidate pool has exhausted its COW frames. This is the first page of that
	// new pool; no Entity Map entries are associated with a LabelSet page.
	struct EntityLabelSetNewPoolRequest {
		SegmentId segment_id{};
		uint64_t pool_owner_id = 0;
		Generation generation{};
		PageId page_id{};
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
	};

	struct EntityLabelSetWritePlan {
		// An empty list means every current Entity-page pool in the pinned
		// snapshot is eligible for typed best-fit placement.
		std::vector<SegmentId> candidate_pool_segments;
		// Pages already staged for COW by an earlier initial-Node plan in the
		// same graph transaction. They are visible only through the old snapshot,
		// so another independent plan must use a fallback instead of emitting a
		// conflicting replacement image for the same logical page.
		std::vector<PageId> excluded_page_ids;
		std::optional<EntityLabelSetAppendRequest> append_page;
		std::optional<EntityLabelSetNewPoolRequest> new_pool;
	};

	struct EntityLabelSetUpdateResult {
		Superblock superblock;
		NodeEntityRecord node;
		EntityLabelOverflowReference previous_overflow;
		EntityLabelOverflowReference current_overflow;
		bool retired_empty_label_page = false;
	};

	// A snapshot-pinned LabelSet rewrite has no publication side effect. The
	// caller combines `replacement_node` with its Entity-page mutation and passes
	// `auxiliary_pages` to EntityPageStore, which verifies this exact Node update
	// before any LabelSet frame can become reachable.
	struct EntityLabelSetMutationPlan {
		uint64_t node_id = 0U;
		EntityPageLocation node_location;
		NodeEntityRecord replacement_node;
		EntityLabelOverflowReference previous_overflow;
		EntityLabelOverflowReference current_overflow;
		bool retired_empty_label_page = false;
		EntityPageAuxiliaryMutationBatch auxiliary_pages;
	};

	// An initial Node record can carry an overflow LabelSet in the same native
	// graph publication as its first Entity Map entry. The enclosing writer adds
	// the matching Node-insertion binding after assigning its logical Node ID.
	struct EntityLabelSetInitialNodePlan {
		NodeEntityRecord node;
		EntityLabelOverflowReference overflow;
		EntityPageAuxiliaryMutationBatch auxiliary_pages;
	};

	// The store creates the typed LabelSet portion of the Node-to-LabelSet
	// publication boundary. EntityPageStore publishes its result with the Node
	// COW image, so a LabelSet slot is never exposed before the Node points at it.
	class EntityLabelSetStore final {
	public:
		EntityLabelSetStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata, SlottedPagePool &pages,
							SharedPagePoolOpenMode openMode = SharedPagePoolOpenMode::SPPOM_RECONCILE);
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] std::vector<uint64_t> read(const MetadataSnapshotPin &snapshot,
												 const EntityLabelOverflowReference &reference,
			const std::function<void(size_t)> &bufferAdmission = {}) const;
		[[nodiscard]] std::vector<uint64_t> readNodeLabels(const MetadataSnapshotPin &snapshot,
																																					 const NodeEntityRecord &node,
			const std::function<void(size_t)> &bufferAdmission = {}) const;
		// Validates the complete label set; inline membership needs no label vector.
		[[nodiscard]] bool containsNodeLabel(const MetadataSnapshotPin &snapshot,
				const NodeEntityRecord &node, uint64_t labelToken,
			const std::function<void(size_t)> &bufferAdmission = {}) const;
		[[nodiscard]] std::vector<std::vector<uint64_t>> readNodeLabelsSorted(
				const MetadataSnapshotPin &snapshot, std::span<const NodeEntityRecord> nodes,
			const std::function<void(size_t)> &resultBufferAdmission = {}) const;
		[[nodiscard]] EntityLabelSetMutationPlan planNodeLabels(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																std::span<const uint64_t> labels,
																const EntityLabelSetWritePlan &plan,
																LogSequenceNumber checkpointLsn) const;
		// Plans labels for a Node that will be inserted by the same Entity-page
		// batch. Inline labels need no auxiliary page. Overflow labels require an
		// explicit append or new-pool fallback because the Node has no current
		// Entity Map location from which best-fit placement can be derived.
		[[nodiscard]] EntityLabelSetInitialNodePlan planInitialNodeLabels(const MetadataSnapshotPin &snapshot,
																		  const NodeEntityRecord &nodeTemplate,
																		  std::span<const uint64_t> labels,
																		  const EntityLabelSetWritePlan &plan,
																		  LogSequenceNumber checkpointLsn) const;

		// Transitional convenience wrapper. New native graph writers should compose
		// `planNodeLabels()` with their wider Entity/Property/derived-state batch.
		[[nodiscard]] EntityLabelSetUpdateResult updateNodeLabels(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																  std::span<const uint64_t> labels,
																  const EntityLabelSetWritePlan &plan,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options = {});

	private:
		struct LoadedNodePage {
			SharedPagePoolContents contents;
			EntityPage page;

			LoadedNodePage(SharedPagePoolContents contents, EntityPage page) :
				contents(std::move(contents)), page(std::move(page)) {}
		};

		struct LoadedLabelPage {
			SharedPagePoolContents contents;
			SlottedPage page;

			LoadedLabelPage(SharedPagePoolContents contents, SlottedPage page) :
				contents(std::move(contents)), page(std::move(page)) {}
		};

		[[nodiscard]] LoadedNodePage loadNodePage(const MetadataSnapshotPin &snapshot, PageId pageId) const;
		[[nodiscard]] LoadedLabelPage loadLabelPage(const MetadataSnapshotPin &snapshot, PageId pageId) const;
		[[nodiscard]] std::vector<SegmentId> eligiblePools(const MetadataSnapshotPin &snapshot,
														   const EntityLabelSetWritePlan &plan) const;
		[[nodiscard]] static NodeEntityRecord makeNodeRecord(const NodeEntityRecord &current,
															 std::span<const uint64_t> labels,
															 const EntityLabelOverflowReference &overflow);
		[[nodiscard]] static EntityLabelOverflowReference asOverflowReference(const SlottedSlotReference &slot);
		static void validateFullLabelSet(std::span<const uint64_t> labels);
		static void validateNodeLabels(const NodeEntityRecord &node, std::span<const uint64_t> labels);
		static void validateExcludedPages(std::span<const PageId> pageIds);
		static void validateAppendRequest(const EntityLabelSetAppendRequest &request);
		static void validateNewPoolRequest(const EntityLabelSetNewPoolRequest &request);

		std::shared_ptr<StorageIO> io_;
		SharedPagePoolAllocator allocator_;
		RelocatableMetadataStore &metadata_;
		SlottedPagePool &pages_;
	};

} // namespace graph::storage
