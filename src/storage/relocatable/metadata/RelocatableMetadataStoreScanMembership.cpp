#include "graph/storage/relocatable/entity/BuildingScanMembership.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipDirectory.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipSource.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDirectoryPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"
#include <bit>

namespace graph::storage {
Superblock RelocatableMetadataStore::installBuildingScanMembershipGroup(const BuildingScanMembershipRequest &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(request); static_cast<void>(checkpointLsn); static_cast<void>(options);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan membership installation requires the isolated format");
#else
    using Map = ScanMembershipDirectoryMap;
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    constexpr size_t maximumBytes = 1024U * 1024U;
    require(request.memory && request.memory->used() <= maximumBytes &&
        request.memory->available() <= maximumBytes - request.memory->used() &&
        request.expected_database_id.isValid() && request.expected_generation.isValid() &&
        request.first_slot.isValid() && request.last_slot >= request.first_slot && request.field_pool_owner_id,
        "BUILDING membership requires a bounded source account, fence and destination range");
    const auto key = ScanMembershipDirectoryCodec::key(request.group_id);
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    const auto validateFence = [&](const Superblock &root) {
        require((root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) &&
            root.database_id == request.expected_database_id && root.generation == request.expected_generation &&
            root.hybrid_property_descriptor_root == request.expected_descriptor,
            "membership installation requires its exact private BUILDING root");
    };
    check();
    utils::OwnedAllocationScope owner(request.memory);
    // Admit the fixed reader frontier before bootstrap or primary-source IO.
    utils::OwnedHeapReservation frontier(request.memory, 32U * 1024U);
    validateFence(bootstrap_.recover()); // refuse complete/foreign files before writer setup
    EntityPageStore entities(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    SlottedPagePool fields(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EntityLabelSetStore labels(io_, *this, fields, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(fields, *this);
    auto writer = beginWrite(true);
    const auto current = bootstrap_.recover(); validateFence(current);
    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
    HybridPropertyDescriptor descriptor = current.hybrid_property_descriptor_root.isValid() ?
        pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor : HybridPropertyDescriptor{};
    descriptor.database_id = current.database_id;
    constexpr size_t ordinal = static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto oldRoot = locations_.findIndexRootOptional(current, index);
    require(bool(oldRoot) == descriptor.roots[ordinal].page_id.isValid(),
        "BUILDING scan directory and descriptor disagree");
    if (oldRoot) {
        Map directory(index, trees, *this, ScanMembershipDirectoryCodec::bind(descriptor, source));
        require(!directory.find(source, key), "BUILDING membership group is already installed");
    }
    // Read every possible primary ID, rather than deriving liveness from a
    // property directory. The terminal group has only 63 representable IDs.
    std::array<uint64_t, 64> ids{};
    const auto width = request.group_id == ScanMembershipDirectoryCodec::MAX_GROUP ? 63U : 64U;
    const uint64_t first = (request.group_id - 1U) * 64U + 1U;
    for (size_t i = 0; i < width; ++i) ids[i] = first + i;
    ScanBlockMembership membership;
    membership.database_id = current.database_id; membership.group_id = request.group_id;
    {
        utils::OwnedHeapReservation sourceBytes(request.memory);
        const auto records = entities.readNodesSorted(source, std::span(ids).first(width),
            [&](size_t bytes) { sourceBytes.set(bytes); });
        require(records.size() == width, "BUILDING membership primary read changed cardinality");
        for (size_t i = 0; i < width; ++i) {
            check(); if (!records[i]) continue;
            const uint64_t bit = UINT64_C(1) << i; membership.live |= bit;
            utils::OwnedHeapReservation labelBytes(request.memory);
            const auto actual = labels.readNodeLabels(source, *records[i], [&](size_t bytes) { labelBytes.set(bytes); });
            for (const auto token : actual) {
                if (membership.labels_from_primary) break;
                auto end = membership.labels.begin() + membership.label_count;
                const auto found = std::lower_bound(membership.labels.begin(), end, token,
                    [](const auto &entry, uint64_t value) { return entry.token < value; });
                if (found == end || found->token != token) {
                    if (membership.label_count == ScanBlockMembership::MAX_LABELS) {
                        membership.labels_from_primary = true; membership.label_count = 0; membership.labels = {};
                        break;
                    }
                    std::move_backward(found, end, end + 1); *found = {token, bit}; ++membership.label_count;
                } else found->members |= bit;
            }
        }
    }
    require(membership.live != 0, "BUILDING membership cannot install an empty primary range");
    check();
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn, request.memory);
    membership.semantic_generation = batch.generation(); membership.validate();
    HybridPropertyPoolPlacement placement(*this, batch, writer, source, request.first_slot, request.last_slot,
        request.memory, check);
    const auto allocatePool = [&](SegmentKind kind, uint64_t poolOwner, size_t count = 1U) {
        return placement.allocate(kind, poolOwner, count, kind == SegmentKind::SGK_PROPERTY_PAGE_POOL,
            kind == SegmentKind::SGK_INDEX_PAGE_POOL && !oldRoot);
    };
    const auto fieldPool = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, request.field_pool_owner_id);
    // Reuse the preceding group's logical page when its authenticated COW image
    // has room. Existing slot references stay unchanged in old and new roots;
    // only the physical page route advances in this same publication.
    auto image = [&]() {
        if (oldRoot && request.group_id > 1U) {
            Map directory(index, trees, *this, ScanMembershipDirectoryCodec::bind(descriptor, source));
            const auto previous = directory.find(source, ScanMembershipDirectoryCodec::key(request.group_id - 1U));
            if (previous) {
                const auto previousSlot = HybridPropertyDirectoryCodec::decodeLocator(*previous);
                HybridPropertyFieldPageStore stored(io_, *this, source);
                auto candidate = stored.cloneForMutation(source, previousSlot.page_id, batch.generation(), request.memory);
                static_cast<void>(candidate.readScanMembership(previousSlot, current.database_id,
                    request.group_id - 1U, descriptor.semantic_generation));
                if (candidate.stats().contiguous_free_bytes >= membership.wireSize() + RSS_SLOTTED_PAGE_SLOT_BYTES)
                    return candidate;
            }
        }
        return HybridPropertyPageImage::create(batch.allocateLogicalPageId(), fieldPool.generation,
            batch.generation(), request.memory);
    }();
    const auto fieldId = image.pageId();
    const auto slot = image.appendScanMembership(membership);
    const Map::Entry entry{key, HybridPropertyDirectoryCodec::locatorValue(slot)};
    utils::OwnedVector<BTreePage> newDirectoryPages{utils::OwnedPmrAllocator<BTreePage>(request.memory)};
    utils::OwnedVector<CatalogEntry> directoryPools{utils::OwnedPmrAllocator<CatalogEntry>(request.memory)};
    utils::OwnedVector<uint64_t> erased{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    IndexRootLocation newRoot;
    if (oldRoot) {
        HybridPropertyDirectoryPreparation prepared(batch, source, *this, trees,
            ScanMembershipDirectoryCodec::bind(descriptor, source), std::span(&entry, 1), std::span<const Map::Key>{},
            request.memory, request.cancellation, allocatePool, check, std::type_identity<Map>{});
        require(bool(prepared.root_), "BUILDING membership insertion lost the directory root");
        newRoot = *prepared.root_;
        for (auto &page : prepared.images_) {
            directoryPools.push_back(page.pool); newDirectoryPages.push_back(std::move(page.page));
        }
        erased.insert(erased.end(), prepared.erased_.begin(), prepared.erased_.end());
    } else {
        const auto pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, index);
        const auto pageId = batch.allocateLogicalPageId();
        BTreePageHeader header; header.index_id = index; header.page_id = pageId; header.generation = pool.generation;
        header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
        const PageOwnerHeader pageOwner{pageId, index, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, checkpointLsn};
        const auto record = Map::encode(entry.key, entry.value);
        BTreePageEntry encoded; encoded.key.assign(record.begin(), record.end());
        newDirectoryPages.push_back(BTreePage::createWithEntries(pageOwner, header, std::span(&encoded, 1)));
        directoryPools.push_back(pool); newRoot = {index, pageId, pool.generation};
    }
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
    const auto writePage = [&](const CatalogEntry &pool, const PageOwnerHeader &pageOwner, const SlottedPage &page, bool native) {
        check();
        utils::OwnedHeapReservation payloadBytes(request.memory, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1024U);
        const auto bytes = page.serializeOwned(request.memory);
        SharedPagePoolPreparedAppend append; append.owner = pageOwner;
        append.free_bytes = page.stats().contiguous_free_bytes; append.payload.assign(bytes.begin(), bytes.end());
        PageLocation location{pageOwner.page_id, pool.segment_id, 0, pool.generation, append.free_bytes};
        placement.write(pool, std::span(&append, 1), std::span(&location, 1), native);
        published.push_back(location);
    };
    const PageOwnerHeader fieldOwner{fieldId, fieldPool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
        RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, fieldPool.generation, checkpointLsn};
    auto bound = std::move(image).bindPrivateOwner(fieldOwner, fieldPool, checkpointLsn);
    writePage(fieldPool, fieldOwner, bound, true);
    for (size_t i = 0; i < newDirectoryPages.size(); ++i) {
        const auto &pool = directoryPools[i]; const auto &page = newDirectoryPages[i];
        const PageOwnerHeader pageOwner{page.header().page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, checkpointLsn};
        writePage(pool, pageOwner, page.slottedPage(), false);
    }
    for (const auto &reservation : placement.reservations_) if (reservation.fresh) {
        allocator_.prepareSetAllocated(batch, reservation.pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&reservation.pool, 1));
    }
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    std::sort(erased.begin(), erased.end()); erased.erase(std::unique(erased.begin(), erased.end()), erased.end());
    locations_.prepareReplacePages(batch, published, erased);
    locations_.prepareReplaceIndexRoots(batch, std::span(&newRoot, 1), {});
    descriptor.roots[ordinal] = {newRoot.root_page_id, newRoot.root_generation};
    const auto added = static_cast<uint64_t>(std::popcount(membership.live));
    require(descriptor.scan_live_node_count <= UINT64_MAX - added && descriptor.scan_membership_group_count < UINT64_MAX,
        "BUILDING membership counts overflow");
    descriptor.scan_live_node_count += added; ++descriptor.scan_membership_group_count;
    descriptor.semantic_generation = batch.generation();
    pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
    batch.hybrid_property_publication_ = true;
    require((batch.candidate().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) != 0,
        "membership preparation lost BUILDING protection");
    check(); publishBatch(batch, writer, options); placement.remember();
    return batch.candidate();
#endif
}
}
