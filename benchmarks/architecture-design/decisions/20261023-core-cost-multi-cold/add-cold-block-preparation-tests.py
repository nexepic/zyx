from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif')
s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockReplacementUpdatesExactMatrixAndRetainsOtherOwnerBytes) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue("replaced")}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        ASSERT_EQ(prepared.pages().size(), 1U); const auto group = prepared.group();
        const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 50U); EXPECT_EQ(matrix.cold_fields, 2U); EXPECT_EQ(matrix.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_rows.roots[0], cold_reference_); EXPECT_EQ(matrix.cold_rows.roots[63], cold_reference_);
        const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
            batch.generation(), batch.generation(), memory);
        EXPECT_TRUE(std::ranges::equal(block.row(0)->root, root));
        const auto old = HybridPropertyColdRowBlock::decode(cold_encoded_bytes_, NODE, 1, before.generation, memory);
        EXPECT_TRUE(std::ranges::equal(block.row(63)->root, old.row(63)->root));
        EXPECT_TRUE(prepared.owners().empty()); EXPECT_TRUE(prepared.extensions().empty());
        EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ClearingAllColdRowsErasesBlockWithoutChangingOwnerLiveness) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<HybridColdRowInput, 2> changes{{{0, 0, false, {}}, {63, 0, false, {}}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory);
        const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
        const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(group.memberships, 48U); EXPECT_EQ(group.live, UINT64_MAX);
        EXPECT_EQ(matrix.cold_fields, 0U); EXPECT_EQ(matrix.cold_rows.owners, 0U);
        EXPECT_EQ(matrix.cold_rows.roots[0], SlottedSlotReference{});
        EXPECT_EQ(matrix.cold_rows.roots[63], SlottedSlotReference{});
        EXPECT_THROW(page.readRecord(cold_reference_), FormatException);
        EXPECT_TRUE(prepared.erasedGroups(NODE).empty());
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationDoesNotDecodeUnrelatedDamagedBlock) {
    split_cold_ = true; bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const PropertyValue same(int64_t(100));
        const auto hot = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1, batch.generation(), memory, &same);
        ASSERT_TRUE(hot); HybridPropertyFieldPageStore fields(io_, *metadata_, pin);
        const auto original = fields.cloneForMutation(pin, field_page_id_, batch.generation(), memory);
        const auto matrix = original.readColumnMembership(hot->sourceGroup().column_membership, hot->sourceGroup(), batch.generation());
        const auto target = matrix.cold_rows.roots[63];
        const auto root = EntityPropertyCollectionRootCodec::encode({64, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{63, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 64}, target, changes, memory);
        const auto &page = image(prepared, field_page_id_);
        EXPECT_TRUE(std::ranges::equal(page.readRecord(cold_reference_), cold_encoded_bytes_));
        EXPECT_THROW(HybridPropertyColdRowBlock::decode(page.readRecord(cold_reference_), NODE, 1, batch.generation(), memory), FormatException);
        EXPECT_EQ(prepared.group().memberships, 51U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationRejectsHotTokenAndDuplicateChangesWithoutWrites) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{1, PropertyValue(int64_t(9))}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        EXPECT_THROW(HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory), FormatException);
        const std::array<HybridColdRowInput, 2> duplicate{{{0, 0, false, {}}, {0, 0, false, {}}}};
        EXPECT_THROW(HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, duplicate, memory), FormatException);
        EXPECT_EQ(batch.candidate().next_logical_page_id, before.next_logical_page_id);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdBlockPreparationAuthenticatesExistingActualLargeRootBeforeReplacement) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const auto root = EntityPropertyCollectionRootCodec::encode({1, graph::Node::typeId, {{2, PropertyValue()}}});
        const std::array<HybridColdRowInput, 1> changes{{{0, 1, false, root}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_, changes, memory, values_.get());
        EXPECT_EQ(prepared.group().memberships, 50U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
