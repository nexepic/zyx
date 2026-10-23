from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualColdSetPublishesCombinedLargeRootAndPreservesOldPin) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("changed-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    const auto committed = metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1));
    EXPECT_GT(committed.generation, source.generation); EXPECT_EQ(memory->used(), 0U);
    {
        HybridPropertyReadContext oldContext(io_, *slotted_pages_, *metadata_, old);
        auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        ASSERT_TRUE(HybridPropertyValueReader::read(oldContext, old, {NODE, 1}, 2, values_.get()).value);
        EXPECT_EQ(HybridPropertyValueReader::read(oldContext, old, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("user-000000"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("changed-id"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualIncompatibleHotSetPublishesAllSurvivingDemotedValues) {
    installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, false}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        auto current = metadata_->openSnapshot(); HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 51U);
        for (uint64_t id = 1; id <= 48; ++id) {
            const auto field = HybridPropertyValueReader::read(context, current, {NODE, id}, 1, values_.get()); ASSERT_TRUE(field.value);
            EXPECT_EQ(field.value->value, id == 1 ? graph::PropertyValue(false) : id == 3 ? graph::PropertyValue() : graph::PropertyValue(int64_t(id + 99)));
        }
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("user-000063"));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(100)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualOwnerDeletionPublishesEntityFieldsCountsAndSurvivingColdValues) {
    installDurableFixture(); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1));
        EXPECT_NO_THROW((void)entities_->readNode(old, 1));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 48U);
        const auto retained = HybridPropertyValueReader::read(context, current, {NODE, 48}, 1, values_.get()); ASSERT_TRUE(retained.value);
        EXPECT_EQ(retained.value->value, graph::PropertyValue(int64_t(147)));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 63U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
