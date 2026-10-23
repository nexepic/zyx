from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, SameBatchColdReplacementRetainsHotValueAndPresenceChangesOnSharedPage) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const bool addPresence : {false, true}) {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const PropertyValue value(int64_t(444)), null;
            auto hot = addPresence ? HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {NODE, 64}, 1, batch.generation(), memory, null) :
                HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, batch.generation(), memory, &value);
            ASSERT_TRUE(hot); const std::array<const HybridPropertyColumnDelta *, 1> hotChanges{&*hot};
            const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue()}}});
            const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlockWithHot(batch, context, hotChanges,
                {NODE, 1}, cold_reference_, changes, memory);
            ASSERT_EQ(prepared.pages().size(), 1U); const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            EXPECT_EQ(group.memberships, addPresence ? 51U : 50U); EXPECT_EQ(matrix.cold_fields, 2U);
            EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(matrix.totalMemberships(), group.memberships);
            const auto column = page.readColumn(hot->columnReference(), hot->replacementColumn().identity, group, batch.generation());
            if (addPresence) {
                EXPECT_TRUE(column.presence & (UINT64_C(1) << 63)); EXPECT_TRUE(column.nulls & (UINT64_C(1) << 63));
                EXPECT_TRUE(matrix.contains({NODE, 64}, 1)); EXPECT_EQ(column.words[0], 100U);
            } else EXPECT_EQ(column.words[0], 444U);
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
                batch.generation(), batch.generation(), memory);
            EXPECT_TRUE(std::ranges::equal(block.row(0)->root, root));
            EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
