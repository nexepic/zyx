from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, ColdBirthAndDeletionKeepOnlyNewOwnerWithOneFinalRoot) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; std::array<EntityPropertyKey, 63> deleted{};
        for (unsigned i = 0; i < 63; ++i) deleted[i] = {NODE, i + 1U};
        graph::utils::OwnedVector<HybridPropertyColumnDelta> plans{graph::utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(memory)};
        plans.reserve(49); std::array<const HybridPropertyColumnDelta *, 49> inputs{};
        for (unsigned i = 0; i < 48; ++i) {
            auto removal = HybridPropertyColumnDelta::planExistingColumn(context, pin, deleted[i], 1, batch.generation(),
                memory, nullptr, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true); ASSERT_TRUE(removal);
            plans.push_back(std::move(*removal)); inputs[i] = &plans.back();
        }
        const PropertyValue value("new-incompatible");
        auto birth = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, value, true); ASSERT_TRUE(birth);
        plans.push_back(std::move(*birth)); inputs[48] = &plans.back();
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true);
        ASSERT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&merged};
        const std::array<HybridColdRowInput, 2> removed{{{0, 0, false, {}}, {62, 0, false, {}}}};
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{1, value}, {2, PropertyValue("new-cold")}}}); const std::array<HybridColdRowInput, 1> added{{{63, 2, false, root}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, removed}, {pending[0], {}, added}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, demotions, deleted, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_C(1) << 63); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 2U);
        EXPECT_FALSE(prepared.erasedGroup()); EXPECT_TRUE(prepared.erasedGroups(NODE).empty());
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_EQ(matrix.cold_rows.owners, group.live); EXPECT_EQ(matrix.cold_fields, 2U);
        const auto ref = matrix.cold_rows.roots[63]; const auto block = HybridPropertyColdRowBlock::decode(
            image(prepared, ref.page_id).readRecord(ref), NODE, 1, batch.generation(), memory);
        ASSERT_TRUE(block.row(63)); EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(block.row(63)->root).entries,
            EntityPropertyCollectionRootCodec::decode(root).entries);
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[at:];p.write_text(s)
