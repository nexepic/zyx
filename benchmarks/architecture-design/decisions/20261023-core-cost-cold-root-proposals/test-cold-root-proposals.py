from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif');s=s[:a]+'''
TEST_F(HybridPropertyColdCompatibleHotTest, ColdRootUsesSameBatchImmutableProposalWithoutReadingUnpublishedManifest) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedHeapReservation stores(memory, 16U * 1024U);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const PropertyCollectionRoot root{1, graph::Node::typeId, {{2, PropertyValue("new-id")}, {4, PropertyValue(std::string(8193, 'y'))}}};
        const auto proposal = HybridPropertyLargeFieldPreparation::prepareRoot(*metadata_, pin, values, {NODE, 1}, root,
            LargeValueId(before.next_large_value_id), memory);
        ASSERT_FALSE(proposal.reused()); const std::span proofs(&proposal, 1);
        const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, proposal.reference()});
        const std::array<HybridColdRowInput, 1> changes{{{0, 2, true, encoded}}};
        for (const bool supplied : {false, true}) {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            EXPECT_EQ(batch.allocateLargeValueId(), proposal.reference().value_id);
            if (!supplied) {
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values), FormatException);
                continue;
            }
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                changes, memory, &values, nullptr, proofs);
            const auto group = prepared.group(); const auto &page = image(prepared, field_page_id_);
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            const auto block = HybridPropertyFieldView::decodeColdRows(page.readRecord(cold_reference_), cold_reference_, group, matrix,
                batch.generation(), batch.generation(), memory);
            ASSERT_TRUE(block.row(0)); EXPECT_TRUE(block.row(0)->large_value_reference);
            EXPECT_TRUE(std::ranges::equal(block.row(0)->root, encoded)); EXPECT_EQ(group.memberships, 51U);
            EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdRootCarrierProofPreservesFieldRootDedupButRejectsHotTokenAndUnreservedIds) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedHeapReservation stores(memory, 16U * 1024U);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        for (const uint32_t token : {4U, 1U}) {
            const PropertyValue value(std::string(8193, 'z'));
            const auto proposal = HybridPropertyLargeFieldPreparation::prepare(*metadata_, pin, values, {NODE, 1}, token, value,
                LargeValueId(before.next_large_value_id), memory);
            ASSERT_FALSE(proposal.reused()); const std::span proofs(&proposal, 1);
            const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({1, graph::Node::typeId, proposal.reference()});
            const std::array<HybridColdRowInput, 1> changes{{{0, 1, true, encoded}}};
            {
                RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch unreserved(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(unreserved, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs), FormatException);
            }
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            EXPECT_EQ(batch.allocateLargeValueId(), proposal.reference().value_id);
            if (token == 1U) {
                EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs), FormatException);
            } else {
                // Field and whole-root carriers use identical owner-neutral
                // bytes. Original batch dedup may select this field proposal.
                const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlock(batch, context, {NODE, 1}, cold_reference_,
                    changes, memory, &values, nullptr, proofs);
                EXPECT_EQ(prepared.group().memberships, 50U);
            }
            EXPECT_EQ(batch.allocatedPageCount(), 0U);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[a:];p.write_text(s)
