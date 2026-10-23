from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativeBirthPublishesLeasedEntityHotNullAndLargeColdRootTogether) {
    live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    {
        auto tx = planner.beginTransaction(); tx.reserveNodeIds(1); ASSERT_EQ(tx.createNodeWithReservedId(64, {}), 64U);
        tx.replaceNodeProperty({64, graph::Node::typeId, {{1, graph::PropertyValue()},
            {2, graph::PropertyValue("new-id")}, {4, graph::PropertyValue(std::string(8193, 'y'))}}});
        ASSERT_TRUE(tx.commit().committed());
    }
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(old, 64)); EXPECT_NO_THROW((void)entities_->readNode(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.columns, 1U); EXPECT_EQ(report.memberships, 54U);
        const auto hot = HybridPropertyValueReader::read(context, current, {NODE, 64}, 1, values_.get()); ASSERT_TRUE(hot.value);
        EXPECT_EQ(hot.value->value, graph::PropertyValue());
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 2, values_.get()).value->value, graph::PropertyValue("new-id"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 64}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'y')));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()).value->value, graph::PropertyValue("user-000000"));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->node_count, 64U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualNativePropertylessEdgeBirthPublishesAdjacencyTypeCountAndLiveness) {
    live_owner_count_ = 63; owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; installDurableFixture(); auto old = metadata_->openSnapshot();
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
    {
        auto tx = planner.beginTransaction(); tx.reserveEdgeIds(1); ASSERT_EQ(tx.createEdgeWithReservedId(64, 1, 2, 7), 64U);
        ASSERT_TRUE(tx.commit().committed());
    }
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findEdgePageLocationOptional(old, 64)); EXPECT_NO_THROW((void)entities_->readEdge(current, 64));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get()).memberships, 51U);
        EXPECT_EQ(context.count({owner_kind_, 64}), std::optional<size_t>(0));
        SystemCatalog catalog(*btree_pages_, *metadata_); ASSERT_TRUE(catalog.readGraphCardinalityStatistics(current));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current)->edge_count, 64U);
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
