from pathlib import Path
r=Path('/workspace/zyx')
p=r/'include/graph/storage/relocatable/allocation/SlottedPagePool.hpp';s=p.read_text();needle='''\tprivate:
\t\tfriend class BTreePageStore;''';replacement='''\tprivate:
        friend class LargeValueStore;
        // Typed readers under the metadata writer reuse their already owned
        // snapshot; attaching must neither acquire a second snapshot nor reconcile.
        SlottedPagePool(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
            const MetadataSnapshotPin &snapshot);
\t\tfriend class BTreePageStore;''';assert needle in s;s=s.replace(needle,replacement,1);p.write_text(s)
p=r/'src/storage/relocatable/allocation/SlottedPagePool.cpp';s=p.read_text();needle='''\tstd::optional<SlottedPagePoolAllocation>
\tSlottedPagePool::allocateEmptyPage''';insert='''    SlottedPagePool::SlottedPagePool(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot) : pool_(std::move(io), metadata, snapshot), metadata_(metadata) {}

''';assert needle in s;s=s.replace(needle,insert+needle,1);p.write_text(s)
p=r/'include/graph/storage/relocatable/property/LargeValueStore.hpp';s=p.read_text();needle='''\tprivate:
\t\tfriend class PropertyCollectionRootStore;''';replacement='''\tprivate:
        friend class HybridPropertyReadContext;
        // Read attachment to an authenticated live source while a metadata
        // writer may be held. This grants no independent publication authority.
        LargeValueStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
            const MetadataSnapshotPin &snapshot);
\t\tfriend class PropertyCollectionRootStore;''';assert needle in s;s=s.replace(needle,replacement,1);p.write_text(s)
p=r/'src/storage/relocatable/property/LargeValueStore.cpp';s=p.read_text();needle='''\tLargeValueStore::~LargeValueStore() = default;''';insert='''    LargeValueStore::LargeValueStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot) : allocator_(io, metadata, snapshot), metadata_(metadata),
        content_index_pages_(new SlottedPagePool(io, metadata, snapshot)),
        content_index_(std::make_unique<BTreePageStore>(*content_index_pages_, metadata)), io_(std::move(io)) {}

''';assert needle in s;s=s.replace(needle,insert+needle,1);p.write_text(s)
p=r/'include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp';s=p.read_text();s=s.replace('''LargeValueStore values(source_io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);''','''LargeValueStore values(source_io_, metadata_, snapshot_);''');p.write_text(s)
