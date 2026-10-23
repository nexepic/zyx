from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.index('TEST_F(HybridPropertyColdCompatibleHotTest, ColdGrowthSplitsAndRelocatesMatrixWithGroupDirectoryEdit)');b=s.index('\n#endif',a);part=s[a:b]
needle='        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch'
part=part.replace(needle,'''        const PropertyValue same(int64_t(100)); const Generation next(before.generation.value() + 1);
        const auto probe = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, next, memory, &same);
        ASSERT_TRUE(probe); HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto initialImage = fields.cloneForMutation(pin, field_page_id_, next, memory);
        const auto initialMatrix = initialImage.readColumnMembership(probe->sourceGroup().column_membership, probe->sourceGroup(), next);
        const auto lastRoot = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> lastChange{{{63, 1, false, lastRoot}}};
        for (const bool modifySecond : {false, true}) {
'''+needle,1)
old='        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);'
new='''        const auto prepared = [&] {
            if (!modifySecond) return HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
            const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
                {{NODE, 1}, cold_reference_, changes}, {{NODE, 64}, initialMatrix.cold_rows.roots[63], lastChange}}};
            return HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory);
        }();'''
assert old in part;part=part.replace(old,new,1)
part=part.replace('        EXPECT_EQ(matrix.cold_rows.roots[63].page_id, field_page_id_);','''        EXPECT_EQ(matrix.cold_rows.roots[63].page_id, field_page_id_);
        if (modifySecond) {
            const auto reference = matrix.cold_rows.roots[63];
            const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, reference.page_id).readRecord(reference), reference,
                group, matrix, batch.generation(), batch.generation(), memory);
            EXPECT_TRUE(std::ranges::equal(block.row(63)->root, lastRoot));
        }''',1)
part=part.replace('        EXPECT_GT(batch.candidate().next_logical_page_id, before.next_logical_page_id);','        EXPECT_GT(batch.candidate().next_logical_page_id, before.next_logical_page_id);\n        }',1)
s=s[:a]+part+s[b:]
a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, MultipleColdBlocksAndHotPresenceShareOneFinalMatrix) {
    split_cold_ = true; seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue null;
        auto hot = HybridPropertyColumnDelta::planMissingColumnMember(context, pin, {NODE, 64}, 1, batch.generation(), memory, null);
        ASSERT_TRUE(hot); const std::array<const HybridPropertyColumnDelta *, 1> hotChanges{&*hot};
        HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto initial = fields.cloneForMutation(pin, field_page_id_, batch.generation(), memory);
        const auto sourceMatrix = initial.readColumnMembership(hot->sourceGroup().column_membership, hot->sourceGroup(), batch.generation());
        const std::array<HybridColdRowInput, 1> first{{{0, 0, false, {}}}}, last{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, first}, {{NODE, 64}, sourceMatrix.cold_rows.roots[63], last}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory, hotChanges);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 49U); EXPECT_EQ(matrix.totalMemberships(), 49U);
        EXPECT_EQ(matrix.cold_fields, 0U); EXPECT_EQ(matrix.cold_rows.owners, 0U); EXPECT_TRUE(matrix.contains({NODE, 64}, 1));
        EXPECT_EQ(group.live, UINT64_MAX); EXPECT_EQ(prepared.pages().size(), 1U);
        const std::array duplicate{blocks[0], blocks[0]};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, duplicate, memory), FormatException);
        EXPECT_EQ(batch.allocatedPageCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
