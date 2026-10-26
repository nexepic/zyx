#include "graph/storage/relocatable/entity/ScanMembershipPublication.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDirectoryPreparation.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"

namespace graph::storage {
std::unique_ptr<PreparedScanMembershipMutation> RelocatableMetadataStore::prepareScanMembershipMutation(
    MetadataWriteBatch &batch, const MetadataWriteGuard &writer, const MetadataSnapshotPin &source,
    const NativeHybridTopologyPublication &proof, const std::shared_ptr<std::pmr::memory_resource> &memory,
    EntityPageStore &entities, EntityLabelSetStore &labels, BTreePageStore &trees) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(batch); static_cast<void>(writer); static_cast<void>(source); static_cast<void>(proof);
    static_cast<void>(memory); static_cast<void>(entities); static_cast<void>(labels); static_cast<void>(trees);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan membership mutation requires its isolated format");
#else
    using Map = ScanMembershipDirectoryMap;
    const auto require = [](bool value, const char *message) { hybrid_property_detail::require(value, message); };
    auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
    require(proof.metadata_ == this && proof.source_ == batch.current() && source.superblock() == batch.current() &&
        ownsSnapshot(source) && proof.scan_pool_owner_id_ && !proof.scan_changes_.empty() && proof.scan_changes_.size() <= 64U &&
        account && account->used() <= 1024U * 1024U && account->available() <= 1024U * 1024U - account->used(),
        "scan membership preparation requires its sealed graph batch and finite source account");
    utils::OwnedAllocationScope allocation(account);
    utils::OwnedHeapReservation frontier(account, 32U * 1024U);
    const auto &current = batch.current();
    auto descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
    constexpr auto ordinal = static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto oldRoot = locations_.findIndexRootOptional(current, index);
    require(bool(oldRoot) == descriptor.roots[ordinal].page_id.isValid(), "scan membership mutation has inconsistent root ownership");
    std::optional<Map> directory;
    if (oldRoot) directory.emplace(index, trees, *this, ScanMembershipDirectoryCodec::bind(descriptor, source));
    auto result = std::unique_ptr<PreparedScanMembershipMutation>(new (account) PreparedScanMembershipMutation(
        *this, batch, writer, current, descriptor, proof.scan_pool_owner_id_, account));
    HybridPropertyFieldPageStore fields(io_, *this, source);
    utils::OwnedHeapReservation boundary(account);
    const auto admit = [&](size_t bytes) { boundary.set(bytes); };
    const auto imageFor = [&](PageId id) -> HybridPropertyPageImage & {
        for (auto &page : result->pages_) if (page.pageId() == id) return page;
        auto image = fields.cloneForMutation(source, id, batch.generation(), account);
        // Existing builders dedicate logical membership pages. Reject a mixed
        // source before field preparation could COW that same logical page.
        const auto bytes = image.serialize();
        const auto raw = SlottedPage::open(bytes, account);
        const auto slots = raw.liveSlotsOwned(account);
        for (const auto &slot : slots) {
            const auto record = raw.readOwned(slot, account);
            require(record.size() >= ScanBlockMembership::HEADER_BYTES &&
                LittleEndianCodec::readU32(record, 0) == ScanBlockMembership::MAGIC,
                "scan mutation cannot independently rewrite a mixed field/membership page");
        }
        result->pages_.push_back(std::move(image));
        return result->pages_.back();
    };
    const auto destination = [&](size_t bytes) -> HybridPropertyPageImage & {
        for (auto &page : result->pages_) {
            const auto stats = page.stats();
            const auto needed = bytes + (stats.reusable_free_slot_count ? 0U : RSS_SLOTTED_PAGE_SLOT_BYTES);
            if (stats.contiguous_free_bytes + stats.reclaimable_bytes < needed) continue;
            if (stats.contiguous_free_bytes < needed) page.compact();
            return page;
        }
        result->pages_.push_back(HybridPropertyPageImage::create(batch.allocateLogicalPageId(), Generation(1),
            batch.generation(), account));
        return result->pages_.back();
    };
    uint64_t previous = 0;
    for (const auto &change : proof.scan_changes_) {
        require(change.id > previous, "sealed scan Node changes are not unique and ordered"); previous = change.id;
    }
    for (size_t begin = 0; begin < proof.scan_changes_.size();) {
        const auto group = 1U + (proof.scan_changes_[begin].id - 1U) / 64U;
        size_t end = begin + 1;
        while (end < proof.scan_changes_.size() && 1U + (proof.scan_changes_[end].id - 1U) / 64U == group) ++end;
        const auto plan = scan_candidate_detail::ScanMembershipMutation::prepare(*this, entities, labels, source,
            group, batch.generation(), proof.scan_changes_.subspan(begin, end - begin), account);
        const auto key = ScanMembershipDirectoryCodec::key(group);
        const auto existing = directory ? directory->find(source, key, std::ref(admit)) : std::nullopt;
        boundary.set(0);
        require(bool(existing) == bool(plan.before), "scan directory disagrees with primary group existence before mutation");
        std::optional<SlottedSlotReference> oldSlot;
        if (existing) {
            oldSlot = HybridPropertyDirectoryCodec::decodeLocator(*existing);
            auto &image = imageFor(oldSlot->page_id);
            const auto actual = image.readScanMembership(*oldSlot, current.database_id, group, descriptor.semantic_generation);
            require(actual.live == plan.before->live && actual.labels_from_primary == plan.before->labels_from_primary &&
                actual.label_count == plan.before->label_count && std::equal(actual.labels.begin(),
                    actual.labels.begin() + actual.label_count, plan.before->labels.begin()),
                "scan mutation source disagrees with complete primary liveness or labels");
            if (plan.after && plan.after->wireSize() <= image.readRecord(*oldSlot).size()) {
                image.replaceScanMembership(*oldSlot, *plan.after, batch.generation());
                result->upserts_.push_back({key, HybridPropertyDirectoryCodec::locatorValue(*oldSlot)});
            } else {
                image.erase(*oldSlot); oldSlot.reset();
            }
        }
        if (!plan.after) result->erases_.push_back(key);
        else if (!oldSlot) {
            auto &page = destination(plan.after->wireSize());
            const auto slot = page.appendScanMembership(*plan.after);
            result->upserts_.push_back({key, HybridPropertyDirectoryCodec::locatorValue(slot)});
        }
        result->live_delta_ += plan.live_node_delta; result->group_delta_ += plan.group_delta;
        begin = end;
    }
    return result;
#endif
}

void RelocatableMetadataStore::materializeScanMembershipMutation(MetadataWriteBatch &batch,
    const MetadataWriteGuard &writer, const MetadataSnapshotPin &source, PreparedScanMembershipMutation &plan,
    BTreePageStore &trees) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(batch); static_cast<void>(writer); static_cast<void>(source); static_cast<void>(plan); static_cast<void>(trees);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan membership publication requires its isolated format");
#else
    using Map = ScanMembershipDirectoryMap;
    const auto require = [](bool value, const char *message) { hybrid_property_detail::require(value, message); };
    require(plan.metadata_ == this && plan.batch_ == &batch && plan.writer_ == &writer && !plan.materialized_ &&
        ownsSnapshot(source) && source.superblock() == plan.source_ && batch.current() == plan.source_,
        "scan membership plan belongs to another source, writer or batch");
    plan.materialized_ = true;
    utils::OwnedAllocationScope allocation(plan.memory_);
    utils::OwnedHeapReservation frontier(plan.memory_, 32U * 1024U);
    constexpr auto ordinal = static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto index = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
    const auto oldRoot = locations_.findIndexRootOptional(plan.source_, index);
    plan.placement_.reset(new HybridPropertyPoolPlacement(*this, batch, writer, source,
        PhysicalSlotId(1), PhysicalSlotId(UINT64_MAX), plan.memory_, [] {}));
    auto &placement = *plan.placement_;
    const auto allocatePool = [&](SegmentKind kind, uint64_t owner, size_t count = 1U) {
        return placement.allocate(kind, owner, count, kind == SegmentKind::SGK_PROPERTY_PAGE_POOL,
            kind == SegmentKind::SGK_INDEX_PAGE_POOL && !oldRoot);
    };
    utils::OwnedVector<BTreePage> directoryPages{utils::OwnedPmrAllocator<BTreePage>(plan.memory_)};
    utils::OwnedVector<CatalogEntry> directoryPools{utils::OwnedPmrAllocator<CatalogEntry>(plan.memory_)};
    utils::OwnedVector<uint64_t> erasedPages{utils::OwnedPmrAllocator<uint64_t>(plan.memory_)};
    std::optional<IndexRootLocation> root;
    if (oldRoot) {
        HybridPropertyDirectoryPreparation prepared(batch, source, *this, trees,
            ScanMembershipDirectoryCodec::bind(plan.descriptor_, source), plan.upserts_, plan.erases_,
            plan.memory_, nullptr, allocatePool, [] {}, std::type_identity<Map>{});
        root = prepared.root_;
        for (auto &image : prepared.images_) { directoryPools.push_back(image.pool); directoryPages.push_back(std::move(image.page)); }
        erasedPages.insert(erasedPages.end(), prepared.erased_.begin(), prepared.erased_.end());
    } else {
        require(!plan.upserts_.empty() && plan.erases_.empty(), "new scan directory has no live groups");
        const auto pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, index);
        const auto id = batch.allocateLogicalPageId();
        BTreePageHeader header; header.index_id = index; header.page_id = id; header.generation = pool.generation;
        header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
        const PageOwnerHeader owner{id, index, PageOwnerKind::POK_INDEX, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
            pool.generation, batch.checkpointLsn()};
        utils::OwnedVector<BTreePageEntry> entries{utils::OwnedPmrAllocator<BTreePageEntry>(plan.memory_)};
        for (const auto &entry : plan.upserts_) {
            const auto encoded = Map::encode(entry.key, entry.value);
            BTreePageEntry pageEntry; pageEntry.key.assign(encoded.begin(), encoded.end()); entries.push_back(std::move(pageEntry));
        }
        directoryPages.push_back(BTreePage::createWithEntries(owner, header, entries)); directoryPools.push_back(pool);
        root = IndexRootLocation{index, id, pool.generation};
    }
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(plan.memory_)};
    const auto writePage = [&](const CatalogEntry &pool, const PageOwnerHeader &owner, const SlottedPage &page, bool native) {
        utils::OwnedHeapReservation payloadCredit(plan.memory_, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1024U);
        const auto bytes = page.serializeOwned(plan.memory_);
        SharedPagePoolPreparedAppend append; append.owner = owner; append.free_bytes = page.stats().contiguous_free_bytes;
        append.payload.assign(bytes.begin(), bytes.end());
        PageLocation location{owner.page_id, pool.segment_id, 0, pool.generation, append.free_bytes};
        placement.write(pool, std::span(&append, 1), std::span(&location, 1), native); published.push_back(location);
    };
    for (auto &image : plan.pages_) {
        if (!image.liveSlotCount()) { erasedPages.push_back(image.pageId().value()); continue; }
        const auto pool = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, plan.owner_);
        const PageOwnerHeader owner{image.pageId(), pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        auto bound = std::move(image).bindPrivateOwner(owner, pool, batch.checkpointLsn()); writePage(pool, owner, bound, true);
    }
    for (size_t i = 0; i < directoryPages.size(); ++i) {
        const auto &pool = directoryPools[i]; const auto &page = directoryPages[i];
        const PageOwnerHeader owner{page.header().page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        writePage(pool, owner, page.slottedPage(), false);
    }
    for (const auto &reservation : placement.reservations_) if (reservation.fresh) {
        allocator_.prepareSetAllocated(batch, reservation.pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&reservation.pool, 1));
    }
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    std::sort(erasedPages.begin(), erasedPages.end()); erasedPages.erase(std::unique(erasedPages.begin(), erasedPages.end()), erasedPages.end());
    if (!published.empty() || !erasedPages.empty()) locations_.prepareReplacePages(batch, published, erasedPages);
    if (root) locations_.prepareReplaceIndexRoots(batch, std::span(&*root, 1), {});
    else locations_.prepareReplaceIndexRoots(batch, {}, std::span(&index, 1));
    auto descriptor = batch.candidate().hybrid_property_descriptor_root == batch.current().hybrid_property_descriptor_root ?
        plan.descriptor_ : pages_.readHybridPropertyDescriptor(batch.candidate().hybrid_property_descriptor_root, batch.candidate()).descriptor;
    require(descriptor.roots[ordinal] == plan.descriptor_.roots[ordinal] &&
        descriptor.scan_live_node_count == plan.descriptor_.scan_live_node_count &&
        descriptor.scan_membership_group_count == plan.descriptor_.scan_membership_group_count,
        "scan membership authority changed before its sealed graph merge");
    const auto add = [&](uint64_t value, int64_t delta) {
        require(delta < 0 ? value >= uint64_t(-delta) : value <= UINT64_MAX - uint64_t(delta), "scan membership count overflow");
        return delta < 0 ? value - uint64_t(-delta) : value + uint64_t(delta);
    };
    descriptor.scan_live_node_count = add(descriptor.scan_live_node_count, plan.live_delta_);
    descriptor.scan_membership_group_count = add(descriptor.scan_membership_group_count, plan.group_delta_);
    descriptor.roots[ordinal] = root ? HybridPropertyTreeRoot{root->root_page_id, root->root_generation} : HybridPropertyTreeRoot{};
    descriptor.semantic_generation = batch.generation();
    if (batch.candidate().hybrid_property_descriptor_root == batch.current().hybrid_property_descriptor_root)
        pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
    else pages_.amendPreparedHybridPropertyDescriptor(batch, descriptor);
    batch.hybrid_property_publication_ = true;
#endif
}
}
