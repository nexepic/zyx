from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();i=s.rindex('#endif');s=s[:i]+'''
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedDeletionRemovesEachNamespacesColdMembership) {
    with_other_owner_kind_ = true; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch; batch.node_deletes.push_back({64, false, 0}); batch.edge_deletes.push_back({64});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_FALSE(metadata_->findNodeLocationOptional(current, 64)); EXPECT_FALSE(metadata_->findEdgeLocationOptional(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            EXPECT_FALSE(context.ownerLiveness({kind, 64}).value()); EXPECT_TRUE(previous.ownerLiveness({kind, 64}).value());
            EXPECT_TRUE(HybridPropertyValueReader::read(context, current, {kind, 1}, 2, values_.get()).value);
        }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, ActualLogicalMixedBirthsKeepIndependentPendingMasksAndSealedLeases) {
    with_other_owner_kind_ = true; live_owner_count_ = 63; installDurableFixture(); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.node_creates.push_back({{}, {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("born-node")}}});
    batch.edge_creates.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1},
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2}, "Type7",
        {{"hot", graph::PropertyValue()}, {"cold", graph::PropertyValue("born-edge")}}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_NO_THROW((void)entities_->readNode(current, 64)); EXPECT_NO_THROW((void)entities_->readEdge(current, 64));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            EXPECT_TRUE(context.ownerLiveness({kind, 64}).value()); EXPECT_FALSE(previous.ownerLiveness({kind, 64}).value());
            EXPECT_EQ(HybridPropertyValueReader::read(context, current, {kind, 64}, 2, values_.get()).value->value,
                graph::PropertyValue(kind == NODE ? "born-node" : "born-edge"));
        }
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[i:];p.write_text(s)
