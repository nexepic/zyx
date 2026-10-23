from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualSharedColdFieldRelocationPreservesBothKindsAndReleasesOnlyAfterOldPin) {
    with_other_owner_kind_ = true; installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    const auto location = metadata_->findPageLocation(old, field_page_id_); const auto pool = metadata_->findCatalog(old, location.segment_id);
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    NativeHybridPropertyPoolRelocation move{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        pool.segment_id, PhysicalSlotId(80), PhysicalSlotId(80), memory};
    static_cast<void>(metadata_->relocateHybridPropertyFieldPool(move, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        const auto moved = metadata_->findPageLocation(current, field_page_id_);
        EXPECT_EQ(moved.page_id, location.page_id); EXPECT_NE(moved.segment_id, location.segment_id);
        EXPECT_EQ(metadata_->findPageLocation(old, field_page_id_), location);
        EXPECT_EQ(metadata_->findCatalog(current, pool.segment_id).state, SegmentState::SGS_RETIRED);
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).memberships, 102U);
        const auto now = HybridPropertyValueReader::read(context, current, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(now.value);
        const auto then = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(then.value);
        EXPECT_EQ(now.value->value, graph::PropertyValue(std::string(8193, 'x'))); EXPECT_EQ(then.value->value, now.value->value);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value,
            graph::PropertyValue("other-first"));
    }
    const auto nextLsn = [&] { return LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1); };
    static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    old.release(); static_cast<void>(metadata_->reclaimRetiredPhysicalSlots(nextLsn()));
    { auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(current, pool.physical_slot)); }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
