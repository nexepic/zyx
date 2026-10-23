from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ReadOnlyGroupProbesDistinguishPendingAndLiveWithoutColdDecoding) {
    live_owner_count_ = 63; installDurableFixture(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::debug::PerfTrace::ScopedObserver observe(&decodes, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += n;
        });
        EXPECT_TRUE(properties_->hasHybridPropertyGroup(pin, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(pin, {NODE, 1}));
        EXPECT_TRUE(properties_->hasHybridPropertyGroup(pin, {NODE, 64})); EXPECT_FALSE(properties_->isHybridPropertyOwner(pin, {NODE, 64}));
        EXPECT_FALSE(properties_->hasHybridPropertyGroup(pin, {NODE, 65}));
        EXPECT_FALSE(properties_->hasHybridPropertyGroup(pin, {EntityPropertyOwnerKind::EPOK_EDGE, 1}));
    }
    EXPECT_EQ(decodes, 0); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualMixedBirthAndDeletionPublishesOnlyNewEntityAndCompleteColdRoot) {
    live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch; batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue("new-incompatible")},
        {"cold", graph::PropertyValue("new-cold")}}});
    for (uint64_t id = 1; id <= 63; ++id) batch.node_deletes.push_back({id, false, 0});
    executeLogical(batch); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= 63; ++id) { EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, id)); EXPECT_NO_THROW((void)entities_->readNode(old, id)); }
        EXPECT_NO_THROW((void)entities_->readNode(current, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(old, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 2U); EXPECT_EQ(report.cold_rows, 1U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 1, values_.get()).value->value, graph::PropertyValue("new-incompatible"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("new-cold"));
        SystemCatalog catalog(*btree_pages_, *metadata_); EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 1U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualFinalGroupDeletionRemovesAllEntityAndFieldAuthorityTogether) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); std::array<EntityPropertyKey, 64> deleted{};
    for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= 64; ++id) { EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, id)); EXPECT_NO_THROW((void)entities_->readNode(old, id)); }
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current); EXPECT_FALSE(context.hasNativeGroups());
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).groups, 0U);
        SystemCatalog catalog(*btree_pages_, *metadata_); EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 0U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualInversePublishesCompleteLargeRowsAndPreservesOldColdSnapshot) {
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    NativeGraphHybridPropertyRootMutation request{source.database_id, source.hybrid_property_descriptor_root,
        NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, NativeGraphHybridPropertyConversion{NODE, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902}};
    request.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    request.conversion->publish_candidate = true; NativeGraphDerivedStateMutation derived;
    static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
        LogSequenceNumber(source.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
        [](auto &) {}, {}, {}, {}, {}, {}, {}, {}, source.generation, {}, {}, {}, {}, {}, request));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(properties_->isHybridPropertyOwner(current, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(old, {NODE, 1}));
        const auto row = properties_->readEntity(current, {NODE, 1}); ASSERT_EQ(row.entries.size(), 3U);
        EXPECT_EQ(row.entries[0].value, graph::PropertyValue(int64_t(100))); EXPECT_EQ(row.entries[1].value, graph::PropertyValue("user-000000"));
        EXPECT_EQ(row.entries[2].value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_TRUE(properties_->readEntityPropertyOptional(current, {NODE, 3}, 1)->value.getType() == graph::PropertyType::NULL_TYPE);
        EXPECT_FALSE(properties_->readEntityOptional(current, {NODE, 49})); EXPECT_NO_THROW((void)entities_->readNode(current, 49));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
