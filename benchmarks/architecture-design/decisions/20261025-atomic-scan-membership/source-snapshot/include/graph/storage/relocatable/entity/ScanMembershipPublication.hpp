#pragma once

#include "graph/storage/relocatable/entity/ScanMembershipMutation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

namespace graph::storage {
    // A metadata-owned, unpublished plan. No caller can supply one or reuse it
    // in another graph batch. It carries no completed-read authority.
    class PreparedScanMembershipMutation final : public utils::OwnedPmrObject {
        friend class RelocatableMetadataStore;
        const RelocatableMetadataStore *metadata_;
        const MetadataWriteBatch *batch_;
        const MetadataWriteGuard *writer_;
        Superblock source_;
        HybridPropertyDescriptor descriptor_;
        uint64_t owner_;
        std::shared_ptr<query::execution::MaterializedByteBudget> memory_;
        utils::OwnedVector<HybridPropertyPageImage> pages_;
        utils::OwnedVector<ScanMembershipDirectoryMap::Entry> upserts_;
        utils::OwnedVector<ScanMembershipDirectoryMap::Key> erases_;
        int64_t live_delta_ = 0, group_delta_ = 0;
        bool materialized_ = false;
        std::unique_ptr<HybridPropertyPoolPlacement> placement_;
        PreparedScanMembershipMutation(const RelocatableMetadataStore &metadata, const MetadataWriteBatch &batch,
            const MetadataWriteGuard &writer, const Superblock &source, HybridPropertyDescriptor descriptor,
            uint64_t owner, std::shared_ptr<query::execution::MaterializedByteBudget> memory)
            : metadata_(&metadata), batch_(&batch), writer_(&writer), source_(source), descriptor_(std::move(descriptor)),
              owner_(owner), memory_(std::move(memory)), pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(memory_)),
              upserts_(utils::OwnedPmrAllocator<ScanMembershipDirectoryMap::Entry>(memory_)),
              erases_(utils::OwnedPmrAllocator<ScanMembershipDirectoryMap::Key>(memory_)) {}
    public:
        ~PreparedScanMembershipMutation() = default;
        PreparedScanMembershipMutation(const PreparedScanMembershipMutation &) = delete;
        PreparedScanMembershipMutation &operator=(const PreparedScanMembershipMutation &) = delete;
    };
}
