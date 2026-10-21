#pragma once
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// Only the authenticated streamed planner can mint this receipt. Unplaced
// images and cardinalities supplied by arbitrary callbacks are insufficient.
class NativeGraphDeferredIndexImages final : public utils::OwnedPmrObject {
    friend class NativeStreamedGraphAdjacencyIndexPlanner;
    friend class NativeStreamedGraphMembershipIndexPlanner;
    friend class NativeGraphWriteTransaction;
    friend class RelocatableMetadataStore;
    const RelocatableMetadataStore *metadata_;
    Generation source_generation_;
    uint64_t index_id_;
    PhysicalPageRef source_data_root_{};
    std::optional<IndexRootLocation> installed_root_;
    std::optional<IndexRootLocation> source_root_;
    std::shared_ptr<std::pmr::memory_resource> memory_;
    utils::OwnedHeapReservation assignments_credit_;
    std::optional<NativeGraphPrivateSegmentSet> private_segments_;
    NativeGraphPrivateSegmentReader reader_;
    NativeGraphIndexTreeMutation fresh_mutation_;
    IndexTreeMultiPoolMutationPreparer fresh_preparer_;
    bool incremental_ = false, materialized_ = false;
    uint64_t entry_count_ = 0;
    size_t page_count_ = 0;
    PhysicalSlotId first_slot_, last_slot_;
    BTreeStructuralMutationPlan plan_;
    utils::OwnedVector<BTreePage> images_;
    utils::OwnedVector<PageLocation> installed_pages_;
    NativeGraphDeferredIndexImages(const RelocatableMetadataStore &metadata, Generation generation, uint64_t indexId,
        std::optional<IndexRootLocation> root, std::shared_ptr<std::pmr::memory_resource> memory,
        PhysicalSlotId first, PhysicalSlotId last, const NativeGraphPrivateSegmentSet &segments,
        const NativeGraphPrivateSegmentReader &reader, PhysicalPageRef dataRoot)
        : metadata_(&metadata), source_generation_(generation), index_id_(indexId), source_data_root_(dataRoot), source_root_(root), memory_(std::move(memory)),
          assignments_credit_(memory_, 2048U), private_segments_(segments), reader_(reader), first_slot_(first), last_slot_(last), images_(utils::OwnedPmrAllocator<BTreePage>(memory_)), installed_pages_(utils::OwnedPmrAllocator<PageLocation>(memory_)) {}
    NativeGraphDeferredIndexImages(const RelocatableMetadataStore &metadata, Generation generation, uint64_t indexId,
        std::optional<IndexRootLocation> root, std::shared_ptr<std::pmr::memory_resource> memory,
        PhysicalSlotId first, PhysicalSlotId last)
        : metadata_(&metadata), source_generation_(generation), index_id_(indexId), source_root_(root), memory_(std::move(memory)),
          assignments_credit_(memory_, 2048U), first_slot_(first), last_slot_(last), images_(utils::OwnedPmrAllocator<BTreePage>(memory_)), installed_pages_(utils::OwnedPmrAllocator<PageLocation>(memory_)) {}
public:
    ~NativeGraphDeferredIndexImages() = default;
    NativeGraphDeferredIndexImages(const NativeGraphDeferredIndexImages &) = delete;
    NativeGraphDeferredIndexImages &operator=(const NativeGraphDeferredIndexImages &) = delete;
};
} // namespace graph::storage
