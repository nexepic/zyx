from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();needle='''class HybridPropertyColdCompatibleHotTest : public graph::storage::test::HybridPropertyColdRowRoutingTest {
protected:
''';assert needle in s;s=s.replace(needle,needle+'''    void expectInversePrepared(HybridPropertyReadContext &context, const MetadataSnapshotPin &pin,
        const std::shared_ptr<MaterializedByteBudget> &memory, int64_t newValues) {
        NativeGraphHybridPropertyConversion conversion{owner_kind_, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902};
        conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        struct Counts { int64_t rows = 0, values = -1; } counts;
        graph::debug::PerfTrace::ScopedObserver observe(&counts, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &c = *static_cast<Counts *>(p);
            if (key == "native_graph.hybrid.inverse_private_new_large_values") c.values = value;
            if (key == "native_graph.hybrid.inverse_private_row_frames") c.rows = value;
        });
        try { HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion); FAIL(); }
        catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION);
            EXPECT_NE(std::string(error.what()).find("inverse native ROW preparation completed"), std::string::npos) << error.what();
        }
        EXPECT_GT(counts.rows, 0); EXPECT_EQ(counts.values, newValues);
    }
''',1);a=s.rindex('#endif');s=s[:a]+'''TEST_F(HybridPropertyColdCompatibleHotTest, NativeInversePlansColdCensusRowInstallationAndRetirementWithoutWrites) {
    seed(); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto fileBytes = io_->fileSize(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
        EXPECT_THROW(context.groupRecord({NODE, 1}), FormatException);
        NativeGraphHybridPropertyConversion conversion{NODE, 1, memory, nullptr, PhysicalSlotId(40), PhysicalSlotId(80), 902};
        conversion.direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
        conversion.publish_candidate = true;
        EXPECT_THROW(HybridPropertyNativeWriteTestAccess::prepareInverse(*metadata_, pin, context, conversion), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), fileBytes);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseAddsHotFieldsToOneActualNewWholeRowCarrier) {
    seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 1);
        const auto old = HybridPropertyValueReader::read(context, pin, {NODE, 1}, 4, values_.get());
        ASSERT_TRUE(old.value); EXPECT_EQ(old.value->value, PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseReusesActualCompleteColdCarrierWithoutAllocatingAnotherValue) {
    cold_only_ = true; seed(true); HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdCompatibleHotTest, NativeInverseRetainsActualEdgeNamespaceWithSameLogicalNodeIds) {
    owner_kind_ = EntityPropertyOwnerKind::EPOK_EDGE; seed();
    HybridPropertyNativeWriteTestAccess::synchronizeFixture(*metadata_);
    auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        expectInversePrepared(context, pin, memory, 0);
        EXPECT_NO_THROW((void)entities_->readEdge(pin, 64)); EXPECT_FALSE(metadata_->findNodePageLocationOptional(pin, 64));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
''' +s[a:];p.write_text(s)
