from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();old='''        SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool, appends, locations, batch.checkpointLsn());
        for (const auto &location : locations) published.push_back(location);''';new='''        // Retained unrelated groups use their dedicated Hybrid pool cursor,
        // separate from the new ordinary ROW frames. Authenticate that complete
        // pool as Hybrid; passing the ordinary writer rejects its real owners.
        const bool native = !appends.empty() && appends.front().owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY;
        require(!native || (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL &&
            std::ranges::all_of(appends, [](const auto &append) { return append.owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY; })),
            "inverse retained Hybrid pool contains unrelated frame kinds");
        SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool, appends, locations,
            batch.checkpointLsn(), 0, native);
        for (const auto &location : locations) published.push_back(location);''';assert s.count(old)==1;s=s.replace(old,new,1);p.write_text(s)
