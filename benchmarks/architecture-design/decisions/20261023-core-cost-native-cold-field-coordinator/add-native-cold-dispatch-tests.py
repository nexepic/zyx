from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();a=s.rindex('#endif')
s=s[:a]+'''TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorMergesColdChangesWithHotNullMissingAndBlockRemoval) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<NativeHybridPropertyColumnReplacement, 3> mixed{{
            {{NODE, 1}, 1, int64_t(777)}, {{NODE, 1}, 2, int64_t(333)}, {{NODE, 1}, 4, std::monostate{}, true}}};
        const std::array<NativeHybridPropertyColumnReplacement, 1> fresh{{{{NODE, 49}, 5, std::monostate{}}}};
        const std::array<NativeHybridPropertyColumnReplacement, 2> removed{{
            {{NODE, 1}, 2, std::monostate{}, true}, {{NODE, 1}, 4, std::monostate{}, true}}};
        const auto prepare = [&](std::span<const NativeHybridPropertyColumnReplacement> cells) {
            NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
                before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
            request.field_lifecycle = true; int64_t frames = 0;
            graph::debug::PerfTrace::ScopedObserver observe(&frames, [](void *p, std::string_view key, int64_t count, bool) noexcept {
                if (key == "native_graph.hybrid.private_field_coordinator_frames") *static_cast<int64_t *>(p) += count;
            });
            try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
            catch (const FormatException &error) {
                EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
                EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
            }
            EXPECT_GT(frames, 0);
        };
        prepare(mixed); prepare(fresh); prepare(removed);
        const std::array<NativeHybridPropertyColumnReplacement, 1> noop{{{{NODE, 49}, 5, std::monostate{}, true}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, noop, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        EXPECT_NO_THROW(HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request));
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        const auto old = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId);
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->entries[0].value, PropertyValue(int64_t(100)));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeCoordinatorSharesOneNewColdRootAcrossOwnersInOriginalImmutablePlanning) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const graph::OwnedPropertyValue value(PropertyValue(std::string(8193, 'q')));
        const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
            {{NODE, 1}, 2, value}, {{NODE, 1}, 4, std::monostate{}, true}, {{NODE, 64}, 2, value}}};
        NativeHybridPropertyColumnWrite request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        request.field_lifecycle = true;
        struct Counts { int64_t values = 0, frames = 0; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t count, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.private_new_field_values") c.values = count;
            if (key == "native_graph.hybrid.private_field_coordinator_frames") c.frames = count;
        });
        try { HybridPropertyNativeWriteTestAccess::prepare(*metadata_, pin, context, request); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("native field coordinator preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_EQ(counts.values, 1); EXPECT_GT(counts.frames, 0);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(io_->fileSize(), fileBytes);
    }
    EXPECT_EQ(memory->used(), 0U);
}
''' +s[a:];p.write_text(s)
