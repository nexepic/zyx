from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');assert at>=0
s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, ColdOwnerLivenessChangesOnlyAfterItsActualRootRemoval) {
    cold_only_ = true; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 64}}};
        const std::array<HybridColdRowInput, 1> rows{{{63, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{NODE, 64}, cold_reference_, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, {}, memory,
            {}, values_.get(), nullptr, {}, {}, deleted), FormatException);
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        const auto group = prepared.group(); EXPECT_EQ(group.live, UINT64_MAX >> 1); EXPECT_EQ(group.memberships, 2U);
        const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
            group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.live, group.live); EXPECT_EQ(matrix.cold_rows.owners, 1U); EXPECT_EQ(matrix.cold_fields, 2U);
        EXPECT_FALSE(prepared.erasedGroup()); EXPECT_TRUE(prepared.owners().empty());
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
        const auto original = HybridPropertyValueReader::read(context, pin, {NODE, 64}, 2, values_.get());
        ASSERT_TRUE(original.value); EXPECT_EQ(original.value->value, PropertyValue("user-000063"));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, ColdOwnerDeletionCannotSilentlyClearOmittedHotFieldsOrRepeatOwners) {
    seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        const std::array<HybridColdRowInput, 1> rows{{{0, 0, false, {}}}};
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> blocks{{{{NODE, 1}, cold_reference_, rows}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, deleted), FormatException);
        const std::array<EntityPropertyKey, 2> duplicate{{{NODE, 1}, {NODE, 1}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, duplicate), FormatException);
        const std::array<EntityPropertyKey, 1> foreign{{{EntityPropertyOwnerKind::EPOK_EDGE, 1}}};
        EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks, memory,
            {}, values_.get(), nullptr, {}, {}, foreign), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, DeletingAllLivePropertylessOwnersErasesGroupAndMembershipTogether) {
    cold_only_ = true; omit_cold_ = true; seed(); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
        std::array<EntityPropertyKey, 64> deleted{}; for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
        const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, {}, memory,
            {}, values_.get(), nullptr, {}, {}, deleted);
        EXPECT_EQ(prepared.group().live, 0U); EXPECT_EQ(prepared.group().memberships, 0U); EXPECT_TRUE(prepared.erasedGroup());
        ASSERT_EQ(prepared.erasedGroups(NODE).size(), 1U); EXPECT_EQ(prepared.erasedGroups(NODE).front(), HybridPropertyDirectoryCodec::groupKey(1));
        EXPECT_TRUE(prepared.groups(NODE).empty());
        EXPECT_THROW((void)image(prepared, field_page_id_).readRecord(membership_reference_), FormatException);
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionCoordinatesEntityAndDemotedSurvivingOwners) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
        ASSERT_TRUE(HybridPropertyValueReader::read(context, pin, {NODE, 48}, 1, values_.get()).value);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionOfAllOwnersCoordinatesFinalGroupCleanup) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        std::array<EntityPropertyKey, 64> deleted{}; for (unsigned i = 0; i < 64; ++i) deleted[i] = {NODE, i + 1U};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_NO_THROW((void)entities_->readNode(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeColdDeletionAuthenticatesExistingLargeRootUnderWriterWithoutNewCarrier) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> deleted{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; request.owner_deletions = deleted; int64_t values = -1;
        graph::debug::PerfTrace::ScopedObserver observer(&values, [](void *p, std::string_view key, int64_t n, bool) noexcept {
            if (key == "native_graph.hybrid.private_new_field_values") *static_cast<int64_t *>(p) = n;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_EQ(values, 0); EXPECT_NO_THROW((void)entities_->readNode(pin, 1));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
}
'''+s[at:];p.write_text(s)
