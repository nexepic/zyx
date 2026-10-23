from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdCompatibleHotTest, PrivatePendingImagesCannotReplaceSealedNativeEntityCreationAuthority) {
    live_owner_count_ = 63; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_); auto pin = metadata_->openSnapshot();
    const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 1> pending{{{NODE, 64}}};
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 64}, 1, int64_t(900)}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.pending_owner_additions = pending;
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_NE(std::string(error.what()).find("sealed new-owner graph publication"), std::string::npos) << error.what();
        }
        EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64)); EXPECT_NO_THROW((void)entities_->readNode(pin, 63));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
'''+s[at:];p.write_text(s)
