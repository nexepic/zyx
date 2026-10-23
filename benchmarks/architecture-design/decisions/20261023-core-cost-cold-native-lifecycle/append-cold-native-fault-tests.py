from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text();old='''    void verifySource() {''';new='''    void verifyColdSetFault(BootstrapPublishFailpoint point) {
        installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
        const graph::OwnedPropertyValue changed(graph::PropertyValue("fault-id"));
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
        NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true;
        MetadataPublishOptions options; options.bootstrap_failpoint = point;
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1), options),
            BootstrapPublishInterrupted);
        EXPECT_EQ(memory->used(), 0U);
        {
            graph::utils::OwnedAllocationScope scope(memory);
            HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
            const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(original.value);
            EXPECT_EQ(original.value->value, graph::PropertyValue("user-000000"));
            auto reopened = RelocatableMetadataStore::openReadOnly(io_);
            SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            LargeValueStore values(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            auto recovered = reopened->openSnapshot(); HybridPropertyReadContext context(io_, pages, *reopened, recovered);
            const bool complete = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
            const auto selected = HybridPropertyValueReader::read(context, recovered, {NODE, 1}, 2, &values); ASSERT_TRUE(selected.value);
            EXPECT_EQ(selected.value->value, graph::PropertyValue(complete ? "fault-id" : "user-000000"));
            const auto retained = HybridPropertyValueReader::read(context, recovered, {NODE, 1}, 4, &values); ASSERT_TRUE(retained.value);
            EXPECT_EQ(retained.value->value, graph::PropertyValue(std::string(8193, 'x')));
            EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, recovered, memory, &values).memberships, 51U);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
    void verifySource() {''';assert old in s;s=s.replace(old,new,1);at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, PartialInactiveRootFaultReopensOnlyOriginalColdValue) {
    verifyColdSetFault(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, CompleteInactiveRootFaultReopensCompleteColdValueAndManifest) {
    verifyColdSetFault(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE);
}
TEST_F(HybridPropertyColdNativeLifecycleTest, CancellationAfterPrivateWritePreservesOldRootRefundsAndAllowsRetry) {
    struct Cancelled final : std::runtime_error { Cancelled() : std::runtime_error("cancel cold private write") {} };
    struct Cancel final : CancellationCheckpoint {
        StorageIO &io; uint64_t epoch; bool armed = true;
        explicit Cancel(StorageIO &source) : io(source), epoch(source.dataMutationEpoch()) {}
        void check() const override { if (armed && io.dataMutationEpoch() != epoch) throw Cancelled(); }
    };
    installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); Cancel cancellation(*io_);
    const graph::OwnedPropertyValue changed(graph::PropertyValue("retry-id"));
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 2, changed}}};
    NativeHybridPropertyColumnWrite request{source.database_id, source.generation, source.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 902}; request.field_lifecycle = true; request.cancellation = &cancellation;
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)), Cancelled);
    EXPECT_NE(io_->dataMutationEpoch(), cancellation.epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), source); cancellation.armed = false;
    static_cast<void>(metadata_->mutateHybridPropertyColumns(request, LogSequenceNumber(source.checkpoint_lsn.value() + 1)));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old), context(io_, *slotted_pages_, *metadata_, current);
        const auto original = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(original.value);
        const auto final = HybridPropertyValueReader::read(context, current, {NODE, 1}, 2, values_.get()); ASSERT_TRUE(final.value);
        EXPECT_EQ(original.value->value, graph::PropertyValue("user-000000")); EXPECT_EQ(final.value->value, graph::PropertyValue("retry-id"));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
