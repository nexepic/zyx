from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp')
s=p.read_text()
a=s.index('    for (const auto id : fieldIds) {',s.index('Superblock RelocatableMetadataStore::publishHybridPropertyRows'))
b=s.index('    utils::OwnedVector<uint64_t> erasedRoots',a)
s=s[:a]+'''    // A converted cohort can pack records from several groups into one page.
    // Authenticate the complete source page, then remove only this group's
    // slots in a private COW image. Other groups retain their logical locators
    // and old snapshots retain the original physical page.
    utils::OwnedVector<HybridPropertyPageImage> retainedFields{
        utils::OwnedPmrAllocator<HybridPropertyPageImage>(conversion.memory)};
    utils::OwnedVector<uint64_t> retiredIds{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
    utils::OwnedVector<uint64_t> supersededIds{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
    for (const auto id : fieldIds) {
        check();
        const auto records = HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, source, id, conversion.memory, &largeValues);
        const auto selected = size_t(std::count_if(references.begin(), references.end(),
            [&](const auto &ref) { return ref.page_id == id; }));
        require(records >= selected, "inverse selected page has fewer records than its selected locators");
        // Erasing also proves distinct, live slot generations; a duplicate
        // locator must fail rather than making a page look fully detached.
        auto image = sourceFields.cloneForMutation(source, id, batch.generation(), conversion.memory);
        for (const auto &reference : references) if (reference.page_id == id) {
            check(); image.erase(reference);
        }
        require(image.liveSlotCount() == records - selected,
            "inverse selected page census disagrees with private slot removal");
        supersededIds.push_back(id.value());
        if (image.liveSlotCount()) retainedFields.push_back(std::move(image));
        else retiredIds.push_back(id.value());
    }
''' +s[b:]
a=s.index('    encodingScratch.set(0);',a)
s=s[:a]+'''    // Allocate preserved hybrid frames only after ROW packing, so the two
    // layouts never share a packing cursor or an owner-kind header.
    std::optional<CatalogEntry> retainedPool;
    size_t retainedPoolFrames = 0;
    for (auto &field : retainedFields) {
        check();
        if (!retainedPool || retainedPoolFrames == RSS_PAGE_POOL_DATA_FRAME_COUNT) {
            retainedPool = newPool(SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id);
            retainedPoolFrames = 0;
        }
        const auto pool = *retainedPool;
        const auto id = field.pageId();
        const PageOwnerHeader owner{id, pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        auto page = std::move(field).bindPrivateOwner(owner, pool, batch.checkpointLsn());
        const auto freeBytes = page.stats().contiguous_free_bytes;
        images.push_back({pool, {id, pool.segment_id, uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + retainedPoolFrames++),
            pool.generation, freeBytes}, std::move(page)});
    }
''' +s[a:]
# Only inverse emission, not another similarly named writer.
a=s.index('    for (const auto &pool : newPools) {',s.index('inverse native ROW preparation completed'))
old='''append.owner = {image.location.page_id, pool.owner_id, PageOwnerKind::POK_PROPERTY, 0,'''
b=s.index(old,a)
s=s[:b]+s[b:].replace(old,'''append.owner = {image.location.page_id, pool.owner_id, image.page.ownerKind(), 0,''',1)
p.write_text(s)
