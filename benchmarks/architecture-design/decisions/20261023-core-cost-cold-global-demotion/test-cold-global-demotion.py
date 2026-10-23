from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif');s=s[:a]+'''TEST_F(HybridPropertyColdCompatibleHotTest, WholeColumnDemotionPreservesAll64OwnerValuesAndColdBindings) {
    hot_owner_count_ = 64; seed(); auto pin = metadata_->openSnapshot(); const auto before = pin.superblock();
    const auto epoch = io_->dataMutationEpoch(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const PropertyValue value("new-category");
        EXPECT_THROW((void)HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1,
            Generation(before.generation.value() + 1), memory, &value), FormatException);
        auto plan = HybridPropertyColumnDelta::planExistingColumn(context, pin, {NODE, 1}, 1,
            Generation(before.generation.value() + 1), memory, &value, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true);
        ASSERT_TRUE(plan); ASSERT_EQ(plan->action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&*plan};
        graph::utils::OwnedVector<std::pmr::vector<std::byte>> encoded{
            graph::utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(memory)};
        graph::utils::OwnedVector<HybridColdRowInput> original, fresh;
        encoded.reserve(64); original.reserve(2); fresh.reserve(62);
        for (uint64_t id = 1; id <= 64; ++id) {
            const PropertyValue scalar = id == 1 ? value : id == 3 ? PropertyValue() : PropertyValue(int64_t(id + 99));
            const PropertyValue name(id == 1 ? "user-000000" : "user-000063"), null;
            std::array<BorrowedPropertyCollectionRootEntry, 3> entries{{{1, &scalar}, {2, &name}, {4, &null}}};
            const size_t count = id == 1 ? 3U : id == 64 ? 2U : 1U;
            encoded.push_back(EntityPropertyCollectionRootCodec::encodeBorrowed(id, graph::Node::typeId,
                std::span(entries).first(count), *memory));
            HybridColdRowInput row{uint8_t(id - 1), uint32_t(count), false, encoded.back()};
            (id == 1 || id == 64 ? original : fresh).push_back(row);
        }
        const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 2> blocks{{
            {{NODE, 1}, cold_reference_, original}, {{NODE, 2}, {}, fresh}}};
        {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const auto prepared = HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, blocks,
                memory, {}, nullptr, nullptr, {}, demotions);
            const auto group = prepared.group(); EXPECT_EQ(group.columns, 0U); EXPECT_EQ(group.memberships, 67U);
            EXPECT_EQ(group.live, UINT64_MAX);
            const auto matrix = image(prepared, group.column_membership.page_id).readColumnMembership(
                group.column_membership, group, batch.generation());
            EXPECT_EQ(matrix.column_count, 0U); EXPECT_EQ(matrix.cold_fields, 67U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
            EXPECT_THROW(image(prepared, plan->columnReference().page_id).readRecord(plan->columnReference()), FormatException);
            for (unsigned slot = 0; slot < 64; ++slot) {
                const auto ref = matrix.cold_rows.roots[slot];
                const auto block = HybridPropertyFieldView::decodeColdRows(image(prepared, ref.page_id).readRecord(ref), ref,
                    group, matrix, batch.generation(), batch.generation(), memory);
                ASSERT_TRUE(block.row(slot)); const auto root = EntityPropertyCollectionRootCodec::decode(block.row(slot)->root);
                EXPECT_EQ(root.owner_entity_id, slot + 1U);
                ASSERT_FALSE(root.entries.empty()); EXPECT_EQ(root.entries[0].key_token, 1U);
                EXPECT_EQ(root.entries[0].value, slot == 0 ? value : slot == 2 ? PropertyValue() : PropertyValue(int64_t(slot + 100)));
                EXPECT_EQ(root.entries.size(), slot == 0 ? 3U : slot == 63 ? 2U : 1U);
            }
        }
        {
            RejectColdHotPhysicalAllocation allocator; MetadataWriteBatch batch(allocator, before,
                LogSequenceNumber(before.checkpoint_lsn.value() + 1));
            const std::array<HybridPropertyDemotionPreparation::ColdBlockChange, 1> incomplete{{{{NODE, 2}, {}, fresh}}};
            EXPECT_THROW((void)HybridPropertyDemotionPreparation::prepareColdBlocks(batch, context, incomplete,
                memory, {}, nullptr, nullptr, {}, demotions), FormatException);
        }
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeWholeColumnDemotionCovers64OwnersAndMergedThresholdRemoval) {
    hot_owner_count_ = 64; seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> replacement{{{{NODE, 1}, 1, changed}}};
        std::vector<NativeHybridPropertyColumnReplacement> removed;
        for (uint64_t id = 1; id <= 17; ++id) removed.push_back({{NODE, id}, 1, std::monostate{}, true});
        for (auto cells : {std::span<const NativeHybridPropertyColumnReplacement>(replacement),
            std::span<const NativeHybridPropertyColumnReplacement>(removed)}) {
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true; int64_t frames = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(frames, 0);
        }
        EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeMissingHotInsertionCanDemoteTheWholeExistingColumn) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue changed(PropertyValue("new-category"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 49}, 1, changed}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true; int64_t frames = 0;
        graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(frames, 0);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
''' +s[a:];p.write_text(s)
