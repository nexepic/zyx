from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowBlock.cpp');s=p.read_text();s+='''
TEST(HybridPropertyColdRowBlockTest, RepackBlocksSplitsGrowthWithoutChangingCanonicalRowsOrSource) {
    Rows original(3); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        const auto source = Block::encode(NODE, 1, Generation(7), original.inputs, memory);
        std::vector<std::vector<std::byte>> roots; std::vector<HybridColdRowInput> changes;
        roots.reserve(3); changes.reserve(3);
        for (unsigned slot = 0; slot < 3; ++slot) {
            std::string text(2100, 'x'); uint32_t state = 12345 + slot;
            for (auto &c : text) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char(33 + state % 90); }
            roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, {{2, PropertyValue(text)}}}));
            changes.push_back({uint8_t(slot), 1, false, roots.back()});
        }
        const auto blocks = Block::repackBlocks(source, NODE, 1, Generation(7), Generation(8), changes, memory);
        ASSERT_EQ(blocks.size(), 3U); uint64_t owners = 0;
        for (const auto &bytes : blocks) {
            const auto block = Block::decode(bytes, NODE, 1, Generation(8), memory);
            EXPECT_EQ(owners & block.owners, 0U); owners |= block.owners;
            EXPECT_LE(block.body.size(), Block::MAX_UNCOMPRESSED_BYTES);
            for (unsigned slot = 0; slot < 3; ++slot)
                if (const auto row = block.row(slot)) EXPECT_TRUE(std::ranges::equal(row->root, roots[slot]));
        }
        EXPECT_EQ(owners, 7U);
        const auto invalid = EntityPropertyCollectionRootCodec::encode({3, graph::Node::typeId, {{2, PropertyValue(std::string(5000, 'x'))}}});
        changes[2].root = invalid; const auto before = memory->used();
        EXPECT_THROW((void)Block::repackBlocks(source, NODE, 1, Generation(7), Generation(8), changes, memory), std::length_error);
        EXPECT_EQ(memory->used(), before);
        EXPECT_EQ(Block::decode(source, NODE, 1, Generation(7), memory).owners, 7U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
''';p.write_text(s)
p=Path('tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp');s=p.read_text();a=s.index('    bool competing_row_');s=s[:a]+'    size_t last_cold_text_bytes_ = 0;\n'+s[a:];s=s.replace('PropertyValue("user-000063")','PropertyValue(last_cold_text_bytes_ ? std::string(last_cold_text_bytes_, \'x\') : std::string("user-000063"))',1);p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, ColdGrowthSplitsAndRelocatesMatrixWithGroupDirectoryEdit) {
    split_cold_ = true; raw_cold_ = true; last_cold_text_bytes_ = 2200; seed();
    auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    std::vector<std::vector<std::byte>> roots; std::vector<HybridColdRowInput> changes;
    roots.reserve(63); changes.reserve(63);
    for (unsigned slot = 0; slot < 63; ++slot) {
        std::string text(2100, 'x'); uint32_t state = 17321 + slot;
        for (auto &c : text) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char(33 + state % 90); }
        roots.push_back(EntityPropertyCollectionRootCodec::encode({slot + 1U, graph::Node::typeId, {{2, PropertyValue(text)}}}));
        changes.push_back({uint8_t(slot), 1, false, roots.back()});
    }
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        const auto group = prepared.group();
        ASSERT_EQ(prepared.groups(NODE).size(), 1U); EXPECT_TRUE(prepared.groups(EntityPropertyOwnerKind::EPOK_EDGE).empty());
        EXPECT_NE(group.column_membership.page_id, field_page_id_);
        const auto groupReference = HybridPropertyDirectoryCodec::decodeLocator(prepared.groups(NODE).front().value);
        EXPECT_EQ(groupReference.page_id, group.column_membership.page_id);
        const auto &groupPage = image(prepared, groupReference.page_id);
        EXPECT_EQ(groupPage.readGroup(groupReference, NODE, 1, batch.generation()), group);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.cold_fields, 64U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
        EXPECT_EQ(group.memberships, 112U); EXPECT_EQ(group.live, UINT64_MAX);
        for (unsigned slot = 0; slot < 63; ++slot) {
            const auto reference = matrix.cold_rows.roots[slot]; EXPECT_NE(reference.page_id, field_page_id_);
            const auto &page = image(prepared, reference.page_id);
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(reference), reference, group, matrix,
                batch.generation(), batch.generation(), memory);
            ASSERT_TRUE(block.row(slot)); EXPECT_TRUE(std::ranges::equal(block.row(slot)->root, roots[slot]));
        }
        EXPECT_EQ(matrix.cold_rows.roots[63].page_id, field_page_id_);
        EXPECT_THROW((void)image(prepared, field_page_id_).readRecord(cold_reference_), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
        EXPECT_GT(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
