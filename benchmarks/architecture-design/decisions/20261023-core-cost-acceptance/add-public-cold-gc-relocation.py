from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdPublicConversion.cpp');s=p.read_text()
a=s.index('TEST_F(HybridPropertyColdPublicConversionTest, ForwardColdConversionPublishesItsNewWholeRootValueAndManifestTogether)')
b=s.index('TEST_F(HybridPropertyColdPublicConversionTest, MissingAdmissionAndInsufficientPeakAllowance',a)
old='''    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''
pos=s.rfind(old,a,b);assert pos>=0
replacement='''    {
        graph::utils::OwnedAllocationScope scope(memory);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueReachabilityReclaimOptions options; options.maximum_manifest_locations = 32;
        options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 32;
        options.maximum_owner_locations = 512; options.maximum_owner_metadata_pages = 512;
        options.maximum_data_pages = 64; options.maximum_retirements = 64; options.maximum_page_locations = 512;
        options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
        LargeValueReachabilityReclaimer gc(values, *slotted_pages_, *metadata_);
        const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
        const auto retired = gc.reclaim(options, nextLsn()); EXPECT_EQ(retired.reachable_values, 1U); EXPECT_EQ(retired.retired_values, 1U);
        PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
        auto wanted = expected(1); wanted.entries[4].value = PropertyValue(std::string(8193, 'l'));
        EXPECT_EQ(roots.readEntity(old, {NODE, 1}), wanted); old.release();
        const auto released = gc.reclaim(options, nextLsn()); EXPECT_EQ(released.retired_values, 0U);
        auto current = metadata_->openSnapshot(); EXPECT_EQ(roots.readEntity(current, {NODE, 1}), wanted);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''
s=s[:pos]+replacement+s[pos+len(old):]
a=s.index('#elif !ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT')
s=s[:a]+'''
TEST_F(HybridPropertyColdPublicConversionTest, RealConvertedFieldPoolMovesWithoutBreakingOtherGroupsOrPinnedRows) {
    prepare(); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); convert(memory);
    auto old = metadata_->openSnapshot(); CatalogEntry pool;
    metadata_->visitCatalogEntries(old, [&](const auto &entry) {
        if (entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && entry.owner_id == 903 && entry.state == SegmentState::SGS_SEALED) pool = entry;
    });
    ASSERT_TRUE(pool.segment_id.isValid()); const auto source = old.superblock();
    NativeHybridPropertyPoolRelocation move{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        pool.segment_id, PhysicalSlotId(80), PhysicalSlotId(80), memory};
    static_cast<void>(metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        auto current = metadata_->openSnapshot(); verify(current, memory); verify(old, memory);
        EXPECT_EQ(metadata_->findCatalog(current, pool.segment_id).state, SegmentState::SGS_RETIRED);
    }
    const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
    static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    old.release(); static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); verify(current, memory); }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[a:];p.write_text(s)
