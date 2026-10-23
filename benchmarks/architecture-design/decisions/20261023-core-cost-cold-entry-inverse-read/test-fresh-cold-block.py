from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowBlock.cpp');s=p.read_text();s+='''
TEST(HybridPropertyColdRowBlockTest, FreshBlocksRequireCanonicalIdentityAndCannotRemoveMissingRows) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    for (const auto kind : {NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        {
            const auto root = EntityPropertyCollectionRootCodec::encode({2,
                kind == NODE ? graph::Node::typeId : graph::Edge::typeId, {{2, PropertyValue()}}});
            const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
            const auto blocks = Block::repackBlocks({}, kind, 1, Generation(7), Generation(8), changes, memory);
            ASSERT_EQ(blocks.size(), 1U); const auto block = Block::decode(blocks.front(), kind, 1, Generation(8), memory);
            EXPECT_EQ(block.owners, 2U); EXPECT_EQ(block.fields, 1U);
            EXPECT_TRUE(std::ranges::equal(block.row(1)->root, root));
            const std::array<HybridColdRowInput, 1> erase{{{1, 0, false, {}}}};
            EXPECT_THROW((void)Block::repackBlocks({}, kind, 1, Generation(7), Generation(8), erase, memory), FormatException);
            EXPECT_THROW((void)Block::repackBlocks({}, kind, 0, Generation(7), Generation(8), changes, memory), FormatException);
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
''';p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, FreshColdBlockInsertionRetainsExistingColdOwnersAndHotAuthority) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({2, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{1, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 2}, {}, changes, memory);
        const auto group = prepared.group();
        const auto &groupPage = image(prepared, group.column_membership.page_id);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 52U); EXPECT_EQ(matrix.cold_fields, 4U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(matrix.cold_rows.roots[63], cold_reference_);
        EXPECT_TRUE(matrix.contains({NODE, 2}, 1)); const auto reference = matrix.cold_rows.roots[1];
        EXPECT_NE(reference.page_id, field_page_id_);
        const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, reference.page_id).readRecord(reference), reference,
            group, matrix, batch.generation(), batch.generation(), memory);
        EXPECT_EQ(block.owners, 2U); EXPECT_TRUE(std::ranges::equal(block.row(1)->root, root));
        EXPECT_TRUE(std::ranges::equal(image(prepared, field_page_id_).readRecord(cold_reference_), cold_encoded_bytes_));
        const std::array<HybridColdRowInput, 1> overwrite{{{0, 1, false, root}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, {}, overwrite, memory), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
