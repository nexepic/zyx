from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, ColdEdgeDeletionPreservesRetainedHotColumnAndOtherOwnerValues) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{owner_kind_, 64}}};
        const std::array<HybridColdRowInput, 1> rows{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{owner_kind_, 64}, cold_reference_, rows}}};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX >> 1); EXPECT_EQ(group.columns, 1U); EXPECT_EQ(group.memberships, 50U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
            group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.presence(1), (UINT64_C(1) << 48) - 1U); EXPECT_EQ(matrix.live, group.live);
        const auto ref = matrix.columnLocator(1); const auto column = HybridPropertyColumnRecord::decode(image(prepared, ref.page_id).readRecord(ref));
        EXPECT_EQ(column.words[0], 100U); EXPECT_EQ(column.words[47], 147U);
        EXPECT_EQ(matrix.cold_fields, 2U); EXPECT_EQ(matrix.cold_rows.owners, 1U);
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
        const auto original = HybridPropertyValueReader::read(context, pin, {owner_kind_, 64}, 2, values_.get());
        ASSERT_TRUE(original.value); EXPECT_EQ(original.value->value, PropertyValue("user-000063"));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionQuotaFailureRefundsAndAllowsOriginalSourceRetry) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.owner_deletions = deleted;
        {
            ASSERT_GT(memory->available(), 96U * 1024U);
            graph::utils::OwnedHeapReservation pressure(memory, memory->available() - 96U * 1024U);
            EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request),
                graph::query::execution::MaterializedByteLimitExceeded);
        }
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
'''+s[at:];p.write_text(s)
