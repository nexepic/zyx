from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, PendingColdRootsAndHotNullShareOneAuthenticatedPrivateGroup) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; const PropertyValue null;
        auto hot = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null, true); ASSERT_TRUE(hot);
        const std::array<const HybridPropertyColumnDelta *, 1> plans{&*hot};
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{2, PropertyValue("new-id")}, {4, PropertyValue()}, {9, PropertyValue(int64_t(900))}}});
        const std::array<HybridColdRowInput, 1> rows{{{63, 3, false, root}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{pending[0], {}, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get()), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            plans, values_.get(), nullptr, {}, {}, {}, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.columns, 1U); EXPECT_EQ(group.memberships, 55U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_TRUE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 6U); EXPECT_TRUE(matrix.cold_rows.owners & (UINT64_C(1) << 63));
        const auto ref = matrix.cold_rows.roots[63]; const auto block = HybridPropertyColdRowBlock::decode(
            image(prepared, ref.page_id).readRecord(ref), NODE, 1, batch.generation(), memory);
        const auto row = block.view().row(63); ASSERT_TRUE(row); EXPECT_EQ(row->field_count, 3U);
        EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(row->root).entries,
            EntityPropertyCollectionRootCodec::decode(root).entries);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, IncompatiblePendingHotFieldDemotesAllOriginalMembersWithItsNewColdRoot) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}}; const PropertyValue incompatible("new-incompatible");
        auto hot = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, incompatible, true); ASSERT_TRUE(hot);
        ASSERT_EQ(hot->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&*hot};
        std::vector<std::vector<std::byte>> roots; roots.reserve(49); std::vector<HybridColdRowInput> rows; rows.reserve(49);
        for (unsigned slot = 0; slot < 48; ++slot) {
            std::vector<PropertyCollectionRootEntry> entries{{1, slot == 2 ? PropertyValue() : PropertyValue(int64_t(slot + 100))}};
            if (!slot) { entries.push_back({2, PropertyValue("user-000000")}); entries.push_back({4, PropertyValue()}); }
            roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, entries}));
            rows.push_back({uint8_t(slot), uint32_t(entries.size()), false, roots.back()});
        }
        roots.push_back(EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId,
            {{1, incompatible}, {2, PropertyValue("new-cold")}}})); rows.push_back({63, 2, false, roots.back()});
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, std::span(rows).first(1)},
            {{NODE, 2}, {}, std::span(rows).subspan(1)}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, demotions, {}, pending);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 53U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.column_count, 0U); EXPECT_EQ(matrix.cold_fields, 53U);
        for (const auto &expected : rows) {
            const auto ref = matrix.cold_rows.roots[expected.owner_slot]; ASSERT_TRUE(ref.isValid());
            const auto block = HybridPropertyColdRowBlock::decode(image(prepared, ref.page_id).readRecord(ref),
                NODE, 1, batch.generation(), memory); const auto row = block.view().row(expected.owner_slot); ASSERT_TRUE(row);
            EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(row->root).entries,
                EntityPropertyCollectionRootCodec::decode(expected.root).entries);
        }
        EXPECT_TRUE(matrix.cold_rows.owners & (UINT64_C(1) << 62)); // Original unrelated cold owner survives.
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[at:];p.write_text(s)
