#pragma once
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
namespace graph::storage {
// Only the laboratory supplies an in-memory candidate descriptor. Normal
// opening always loads the pinned durable root and keeps its nonempty guard.
class HybridPropertyReadContextTestAccess {
public:
    static std::optional<HybridPropertyGroupRecord> group(const HybridPropertyReadContext &context,
        EntityPropertyKey owner) { return context.findGroup(owner, {}, nullptr, true, true); }
    static HybridPropertyColumnMembership membership(const HybridPropertyReadContext &context,
        const HybridPropertyGroupRecord &group) { return context.columnMembership(group); }
    static HybridPropertyReadContext open(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        const HybridPropertyDescriptor &descriptor, const CancellationCheckpoint *cancellation = nullptr) {
        const HybridPropertyDescriptorPage frame{snapshot.superblock().generation,
            snapshot.superblock().checkpoint_lsn, descriptor};
        const auto decoded = HybridPropertyDescriptorCodec::decode(HybridPropertyDescriptorCodec::encode(frame),
            descriptor.database_id, frame.generation, frame.generation, frame.committed_lsn);
        return HybridPropertyReadContext(std::move(io), pages, metadata, snapshot, decoded.descriptor, cancellation);
    }
};
}
