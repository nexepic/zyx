from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();idx=s.rindex('#endif')
t='''
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedNamespaceColdSetSharesOnePrivateFrameAndPreservesOldPins) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    const auto source = old.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 2, std::string("node-new")},
        {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, std::string("edge-new")}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            const auto read = [&](HybridPropertyReadContext &reader, const MetadataSnapshotPin &pin, uint64_t id, uint32_t token) {
                auto field = HybridPropertyValueReader::read(reader, pin, {kind, id}, token, values_.get());
                EXPECT_TRUE(field.value); return field.value ? field.value->value : graph::PropertyValue();
            };
            EXPECT_EQ(read(context, current, 1, 2), graph::PropertyValue(kind == NODE ? "node-new" : "edge-new"));
            EXPECT_EQ(read(previous, old, 1, 2), graph::PropertyValue(kind == NODE ? "user-000000" : "other-first"));
            EXPECT_EQ(read(context, current, 64, 2), graph::PropertyValue(kind == NODE ? "user-000063" : "other-last"));
            EXPECT_EQ(read(context, current, 1, 1), graph::PropertyValue(int64_t(kind == NODE ? 100 : 200)));
        }
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 2U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedHotAndColdSetMergesCompatibleBaseBeforeColdRepacking) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    const auto source = old.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, int64_t(999)},
        {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, std::string("edge-new")}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(999)));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("edge-new"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''
p.write_text(s[:idx]+t+s[idx:])
