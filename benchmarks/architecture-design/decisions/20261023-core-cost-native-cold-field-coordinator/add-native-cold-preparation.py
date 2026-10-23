from pathlib import Path
r=Path('/workspace/zyx');p=r/'include/graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp';s=p.read_text();s=s.replace('    class HybridPropertyPoolPlacement;', '    class HybridPropertyPoolPlacement;\n    class HybridPropertyReadContext;',1)
s=s.replace('        friend class HybridPropertyDescriptorStoreTestAccess;','        friend class HybridPropertyDescriptorStoreTestAccess;\n        friend class HybridPropertyNativeWriteTestAccess;')
s=s.replace('''            std::span<const NativeGraphLargeValueMutation> jointValues = {});
        [[nodiscard]] Superblock publishHybridPropertyRows''','''            std::span<const NativeGraphLargeValueMutation> jointValues = {},
            HybridPropertyReadContext *preparationSource = nullptr);
        [[nodiscard]] Superblock publishHybridPropertyRows''');p.write_text(s)
p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp';s=p.read_text();a=s.index('bool RelocatableMetadataStore::prepareHybridPropertyColumns');b=s.index('Superblock RelocatableMetadataStore::',a+10);chunk=s[a:b]
chunk=chunk.replace('''std::span<const NativeGraphLargeValueMutation> jointValues) {''','''std::span<const NativeGraphLargeValueMutation> jointValues,
    HybridPropertyReadContext *preparationSource) {''',1)
chunk=chunk.replace('''    HybridPropertyReadContext context(io_, sourcePages, *this, source, request.cancellation);''','''    // A private laboratory source can exercise the original coordinator up to
    // its physical-write boundary. It grants no publication authority and must
    // retain this exact pin/store/file; normal callers always use durableSource.
    HybridPropertyReadContext durableSource(io_, sourcePages, *this, source, request.cancellation);
    auto &context = preparationSource ? *preparationSource : durableSource;
    require(!preparationSource || (context.usesSnapshot(source) && &context.metadata_ == this &&
        context.source_io_ == io_ && !hybridVectors && !hybridTopology && jointValues.empty() &&
        request.owner_deletions.empty() && request.pending_owner_additions.empty()),
        "native private field preparation has a foreign source or unsupported compound work");
    const auto sourceGroupFor = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
        SlottedSlotReference *reference = nullptr) {
        return preparationSource ? context.findGroup(owner, admission, reference, true, true) :
            context.groupRecord(owner, admission, reference);
    };''',1)
chunk=chunk.replace('context.groupRecord(owner, std::ref(admit), &groupReference)','sourceGroupFor(owner, std::ref(admit), &groupReference)').replace('context.groupRecord(replacement.owner, std::ref(admit))','sourceGroupFor(replacement.owner, std::ref(admit))')
chunk=chunk.replace('''lifecycle->erasedColumns(kind).empty() && lifecycle->erasedExtensions(kind).empty() && !erasedAnyGroup &&''','''lifecycle->erasedColumns(kind).empty() && lifecycle->erasedExtensions(kind).empty() &&
            lifecycle->groups(kind).empty() && !erasedAnyGroup &&''')
needle='''    const auto writeImages = [&]<class Images>'''
assert needle in chunk
chunk=chunk.replace(needle,'''    if (preparationSource) {
        debug::PerfTrace::addValue("native_graph.hybrid.private_field_coordinator_frames", int64_t(fieldImages.size()));
        debug::PerfTrace::addValue("native_graph.hybrid.private_field_coordinator_directories", int64_t(indexImages.size()));
        check();
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
            "native field coordinator preparation completed; private source cannot publish");
    }
    const auto writeImages = [&]<class Images>''',1)
s=s[:a]+chunk+s[b:];p.write_text(s)
p=r/'tests/include/storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp';p.write_text('''#pragma once
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
namespace graph::storage {
// Laboratory-only access to the original coordinator. A supplied candidate
// source is unconditionally refused before its first physical write.
class HybridPropertyNativeWriteTestAccess {
public:
    static void prepare(RelocatableMetadataStore &metadata, const MetadataSnapshotPin &source,
        HybridPropertyReadContext &context,
        const NativeHybridPropertyColumnWrite &request) {
        const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
        auto writer = metadata.beginWrite(true);
        MetadataWriteBatch batch(metadata.metadata_page_pool_, source.superblock(),
            LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1), request.memory);
        SlottedPagePool pages(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, metadata);
        LargeValueStore values(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityPageStore entities(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityLabelSetStore labels(metadata.io_, metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyPoolPlacement placement(metadata, batch, writer, source, request.first_slot,
            request.last_slot, request.memory, check);
        static_cast<void>(metadata.prepareHybridPropertyColumns(batch, writer, source, request, request.replacements,
            pages, trees, values, entities, labels, placement, nullptr, nullptr, {}, &context));
    }
};
}
''')
# Placement constructor remains private to metadata and the laboratory access.
p=r/'include/graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp';s=p.read_text().replace('    friend class RelocatableMetadataStore;','    friend class RelocatableMetadataStore;\n    friend class HybridPropertyNativeWriteTestAccess;',1);p.write_text(s)
