from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();i=s.rindex('#endif');s=s[:i]+'''
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedDemotionAndColdSetMigratesOnlyOneGroupsOwners) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{
        {{NODE, 1}, 1, false}, {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, graph::OwnedPropertyValue(graph::PropertyValue("new-edge"))}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        for (uint64_t id = 1; id <= 48; ++id) {
            const auto field = HybridPropertyValueReader::read(context, current, {NODE, id}, 1, values_.get()); ASSERT_TRUE(field.value);
            EXPECT_EQ(field.value->value, id == 1 ? graph::PropertyValue(false) : id == 3 ? graph::PropertyValue() : graph::PropertyValue(int64_t(id + 99)));
        }
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("new-edge"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 48}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(247)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedDemotionsCannotDoubleTheOriginal64OwnerLimit) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto epoch = io_->dataMutationEpoch();
    const std::array<NativeHybridPropertyColumnReplacement, 2> cells{{{{NODE, 1}, 1, false}, {{EntityPropertyOwnerKind::EPOK_EDGE, 1}, 1, false}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), source); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedCollectionsUseTheSameAtomicColdCoordinator) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"hot", graph::PropertyValue(int64_t(1010))},
        {"cold", graph::PropertyValue("logical-node")}, {"nullable", graph::PropertyValue()}}});
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_EDGE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {{"hot", graph::PropertyValue(int64_t(2020))}, {"cold", graph::PropertyValue("logical-edge")}}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("logical-node"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {EntityPropertyOwnerKind::EPOK_EDGE, 1}, 2, values_.get()).value->value, graph::PropertyValue("logical-edge"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[i:];p.write_text(s)
