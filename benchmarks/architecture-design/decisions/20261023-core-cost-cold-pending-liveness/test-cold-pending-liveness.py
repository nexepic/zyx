from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, PendingColdOwnerHotNullUsesActualAbsentEntityAndOnePrivateMembership) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue null; const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}};
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null), FormatException);
        auto plan = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, pending[0], 1,
            batch.generation(), memory, null, true); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->effects().set_live, UINT64_C(1) << 63); EXPECT_EQ(plan->effects().memberships, 1);
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin); const std::array<const HybridPropertyColumnDelta *, 1> plans{&*plan};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            plans, pending, memory), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            plans, pending, memory, nullptr, {}, true);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(group.columns, 1U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_TRUE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_EQ(matrix.cold_rows.owners, 1U | (UINT64_C(1) << 62)); EXPECT_FALSE(matrix.cold_rows.owners & (UINT64_C(1) << 63));
        const auto column = image(prepared, plan->columnReference().page_id).readColumn(plan->columnReference(),
            plan->replacementColumn().identity, group, batch.generation());
        EXPECT_TRUE(column.nulls & (UINT64_C(1) << 63)); EXPECT_EQ(column.words[63], 0U);
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty()); EXPECT_FALSE(prepared.erasedGroup());
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_THROW(context.groupRecord(pending[0]), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, PendingPropertylessColdEdgeChangesLivenessWithoutFabricatedFields) {
    live_owner_count_ = 63; owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin); const std::array<EntityPropertyKey, 1> pending{{{owner_kind_, 64}}};
        const auto prepared = HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            {}, pending, memory, nullptr, {}, true);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(group.memberships, 51U); EXPECT_EQ(group.columns, 1U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_FALSE(matrix.contains(pending[0], 1)); EXPECT_EQ(matrix.cold_fields, 3U);
        EXPECT_FALSE(matrix.cold_rows.owners & (UINT64_C(1) << 63)); EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_TRUE(std::ranges::equal(image(prepared, cold_reference_.page_id).readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 63)); EXPECT_FALSE(metadata_->findEdgePageLocationOptional(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdPendingLivenessRejectsActualLiveDuplicateAndForeignOwnerClaims) {
    live_owner_count_ = 63; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        for (const auto owner : {EntityPropertyKey{NODE, 1}, EntityPropertyKey{NODE, 65},
            EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, 64}}) {
            const std::array<EntityPropertyKey, 1> pending{owner};
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
                {}, pending, memory, nullptr, {}, true), FormatException);
        }
        const std::array<EntityPropertyKey, 2> duplicate{{{NODE, 64}, {NODE, 64}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, pin, fields,
            {}, duplicate, memory, nullptr, {}, true), FormatException);
        const PropertyValue value(int64_t(900));
        EXPECT_THROW((void)HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, pin, {NODE, 1}, 1,
            batch.generation(), memory, value, true), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
'''+s[at:];p.write_text(s)
