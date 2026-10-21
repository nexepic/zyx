#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyNativeJournalTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    LargeValueMaintenanceJournalStoreOptions journalOptions() { return {{PhysicalSlotId(90), PhysicalSlotId(100), 904}, 77}; }
    LargeValueReachabilityReclaimOptions reclaimOptions() {
        LargeValueReachabilityReclaimOptions options;
        options.maximum_manifest_locations = 1; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
        options.maximum_owner_locations = 2; options.maximum_owner_metadata_pages = 8192; options.maximum_data_pages = 128;
        options.maximum_retirements = 256; options.maximum_page_locations = 4096; options.maximum_pools = 64;
        options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
        options.first_journal_state_slot = PhysicalSlotId(90); options.last_journal_state_slot = PhysicalSlotId(100);
        options.journal_state_page_owner_id = 904; options.journal_state_id = 77; return options;
    }
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902}; return options;
    }
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
    void seedMixed(LargeValueStore &values, PropertyCollectionRootStore &roots, const std::shared_ptr<MaterializedByteBudget> &memory) {
        seed(NODE, 65, 0);
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
        auto tx = planner.beginTransaction(); tx.replaceNodeProperty({65, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'r'))}}});
        const auto edge = tx.createEdge(1, 2, 7); tx.replaceEdgeProperty({edge, graph::Edge::typeId, {{1, PropertyValue(std::string(8192, 'e'))}}});
        ASSERT_TRUE(tx.commit().committed()); ASSERT_NO_THROW(convert(memory));
        const auto state = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
            {{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))},
            {{NODE, 1}, 2, OwnedPropertyValue(PropertyValue(std::string(8192, 'y')))},
            {{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'z')))}}};
        NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    }
};
}
TEST_F(HybridPropertyNativeJournalTest, RowJournalKeepsItsExistingStateWireAndOldSnapshotContract) {
    seed(NODE, 2, 1); StateRecordStore states(*slotted_pages_, *metadata_);
    LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions()); auto old = metadata_->openSnapshot();
    LargeValueMaintenanceJournal journal; journal.scan_generation = old.superblock().generation;
    const auto saved = store.saveAfterScan(old, journal); auto pin = metadata_->openSnapshot();
    EXPECT_EQ(saved.journal.scan_generation, pin.superblock().generation); EXPECT_EQ(store.load(pin), saved.journal);
    EXPECT_FALSE(store.load(old)); EXPECT_EQ(LargeValueMaintenanceJournalCodec::decode(
        states.read(pin, journalOptions().state_id).inline_bytes), saved.journal);
}
TEST(NativeJournalEvidenceBudgetTest, DecodedIdsRetainAdmissionAcrossScopeAndRefundAfterTheirOwnerDies) {
    LargeValueMaintenanceJournal journal; journal.scan_generation = Generation(1);
    journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
    journal.manifest_scan_exhausted = true; journal.manifest_after_value_id = 128;
    for (uint64_t i = 1; i <= 128; ++i) { journal.candidate_value_ids.push_back(LargeValueId(i)); journal.reachable_value_ids.push_back(LargeValueId(i)); }
    const auto bytes = LargeValueMaintenanceJournalCodec::encode(journal);
    auto memory = std::make_shared<MaterializedByteBudget>(8192); std::optional<LargeValueMaintenanceJournal> retained;
    { graph::utils::OwnedAllocationScope scope(memory); retained.emplace(LargeValueMaintenanceJournalCodec::decode(bytes)); EXPECT_GT(memory->used(), 0U); }
    EXPECT_GT(memory->used(), 0U); EXPECT_EQ(*retained, journal); retained.reset(); EXPECT_EQ(memory->used(), 0U);
    auto tiny = std::make_shared<MaterializedByteBudget>(128);
    { graph::utils::OwnedAllocationScope scope(tiny); EXPECT_THROW((void)LargeValueMaintenanceJournalCodec::decode(bytes), graph::query::execution::MaterializedByteLimitExceeded); }
    EXPECT_EQ(tiny->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, GenericStateWriterCannotCreateTheReservedNativeEvidenceIdentity) {
    seed(NODE, 2, 1); StateRecordStore states(*slotted_pages_, *metadata_);
    LargeValueMaintenanceJournal journal; const auto before = metadata_->openSnapshot().superblock(); journal.scan_generation = before.generation;
    StateRecord forged; forged.state_id = 77; forged.key = std::string(RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
    forged.inline_bytes = LargeValueMaintenanceJournalCodec::encode(journal); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)states.upsert(forged, journalOptions().state_record_options), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyNativeJournalTest, NativeJournalRequiresAdmissionAndOnlyWritesItsCanonicalStateIdentity) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    auto source = metadata_->openSnapshot(); LargeValueMaintenanceJournal journal; journal.scan_generation = source.superblock().generation;
    const auto before = source.superblock(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)store.saveAfterScan(source, journal), FormatException);
    auto tiny = std::make_shared<MaterializedByteBudget>(128);
    { graph::utils::OwnedAllocationScope scope(tiny); EXPECT_THROW((void)store.saveAfterScan(source, journal), graph::query::execution::MaterializedByteLimitExceeded); }
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    { graph::utils::OwnedAllocationScope scope(memory); ASSERT_NO_THROW((void)store.saveAfterScan(source, journal)); }
    EXPECT_EQ(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
    StateRecord forged; forged.state_id = 78; forged.key = "other"; forged.inline_bytes = {std::byte{1}};
    EXPECT_THROW((void)states.upsert(forged, journalOptions().state_record_options), FormatException);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, UnknownStateRefusesColumnFieldAndMapUpdatesBeforeAnyPublication) {
    seed(NODE, 2, 1); StateRecordStore states(*slotted_pages_, *metadata_);
    StateRecord foreign; foreign.state_id = 77; foreign.key = "unrecognized-derived-owner";
    foreign.inline_bytes = {std::byte{3}};
    ASSERT_NO_THROW((void)states.upsert(foreign, journalOptions().state_record_options));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    auto source = metadata_->openSnapshot(); const auto before = source.superblock(); const auto epoch = io_->dataMutationEpoch();
    const auto original = properties_->readEntity(source, {NODE, 1});
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, int64_t(999)}}};
    for (const bool lifecycle : {false, true}) {
        NativeHybridPropertyColumnWrite write{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, lifecycle};
        EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(memory->used(), 0U);
    }
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
    auto tx = planner.beginTransaction(); auto changed = original; changed.entries[0].value = PropertyValue(int64_t(999));
    ASSERT_NO_THROW(tx.stageHybridPropertyMaps(std::span(&changed, 1), NativeHybridPropertyMapMode::NHPMM_REPLACE, memory));
    EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(properties_->readEntity(metadata_->openSnapshot(), {NODE, 1}), original);
    EXPECT_EQ(states.read(metadata_->openSnapshot(), 77), foreign); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, NativeJournalRefusesAnExistingForeignStateRecordBeforeAnyWrite) {
    seed(NODE, 2, 1); StateRecordStore states(*slotted_pages_, *metadata_);
    StateRecord foreign; foreign.state_id = 77; foreign.key = "foreign"; foreign.inline_bytes = {std::byte{3}};
    ASSERT_NO_THROW((void)states.upsert(foreign, journalOptions().state_record_options));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions()); auto source = metadata_->openSnapshot();
    LargeValueMaintenanceJournal journal; journal.scan_generation = source.superblock().generation;
    const auto before = source.superblock(); const auto epoch = io_->dataMutationEpoch();
    { graph::utils::OwnedAllocationScope scope(memory); EXPECT_THROW((void)store.saveAfterScan(source, journal), FormatException); }
    const std::array<EntityPropertyKey, 1> removed{{{NODE, 1}}};
    NativeHybridPropertyColumnWrite deletion{before.database_id, before.generation, before.hybrid_property_descriptor_root,
        {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true, removed};
    EXPECT_THROW((void)metadata_->mutateHybridPropertyColumns(deletion, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(states.read(metadata_->openSnapshot(), 77), foreign); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, StateCOWReusesUnpinnedFramesBeyondOnePoolAndKeepsTheOldJournalReadable) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    {
        graph::utils::OwnedAllocationScope scope(memory); auto source = metadata_->openSnapshot();
        LargeValueMaintenanceJournal initial; initial.scan_generation = source.superblock().generation;
        (void)store.saveAfterScan(source, initial); source.release();
        auto old = metadata_->openSnapshot(); const auto payload = store.load(old);
        const auto location = metadata_->findStateLocation(old, 77); const auto original = metadata_->findPageLocation(old, location.page_id);
        const auto descriptor = old.superblock().hybrid_property_descriptor_root;
        for (unsigned step = 0; step < 70; ++step) {
            auto pin = metadata_->openSnapshot(); auto journal = *store.load(pin); ASSERT_EQ(journal.scan_generation, pin.superblock().generation);
            (void)store.saveAfterScan(pin, journal);
        }
        auto current = metadata_->openSnapshot(); const auto route = metadata_->findPageLocation(current, location.page_id);
        EXPECT_EQ(route.segment_id, original.segment_id); EXPECT_EQ(metadata_->findStateLocation(current, 77), location);
        EXPECT_EQ(current.superblock().hybrid_property_descriptor_root, descriptor); EXPECT_EQ(store.load(old), payload);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, NativeJournalABRecoverySelectsOneWholeStateAndPreservesItsOldFrame) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    {
        graph::utils::OwnedAllocationScope scope(memory); auto first = metadata_->openSnapshot();
        LargeValueMaintenanceJournal journal; journal.scan_generation = first.superblock().generation;
        (void)store.saveAfterScan(first, journal); first.release(); auto old = metadata_->openSnapshot(); const auto payload = store.load(old);
        for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
            auto source = metadata_->openSnapshot(); auto current = *store.load(source); MetadataPublishOptions interruption; interruption.bootstrap_failpoint = point;
            EXPECT_THROW((void)store.saveAfterScan(source, current, interruption), BootstrapPublishInterrupted);
            auto recovered = RelocatableMetadataStore::openReadOnly(io_); auto pin = recovered->openSnapshot();
            SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY); StateRecordStore recoveredStates(pages, *recovered);
            LargeValueMaintenanceJournalStore recoveredStore(recoveredStates, *recovered, journalOptions());
            EXPECT_EQ(recoveredStore.load(pin)->scan_generation, pin.superblock().generation);
            EXPECT_EQ(pin.superblock().generation.value(), source.superblock().generation.value() + size_t(point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE));
            EXPECT_EQ(store.load(old), payload); EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, old.superblock().hybrid_property_descriptor_root);
            EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan());
        }
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, IncrementalNativeGCResumesAcrossWindowsAndRetiresOnlyTheOrphan) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    auto old = metadata_->openSnapshot(); const auto previous = roots.readEntity(old, {NODE, 2}); const auto state = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'w')))}}};
    NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    const auto expected = roots.readEntity(metadata_->openSnapshot(), {NODE, 2});
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_); auto options = reclaimOptions();
    size_t retired = 0, windows = 0; bool complete = false;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        for (unsigned step = 0; step < 128 && !complete; ++step) {
            auto snapshot = metadata_->openSnapshot(); auto stored = store.load(snapshot); LargeValueMaintenanceJournal journal;
            if (stored && stored->scan_generation == snapshot.superblock().generation) journal = *stored;
            else journal.scan_generation = snapshot.superblock().generation;
            auto advanced = reclaimer.advanceJournal(snapshot, journal, options);
            if (advanced.completed_without_candidates) { complete = true; break; }
            if (journal.phase != LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM) {
                (void)store.saveAfterScan(snapshot, advanced); continue;
            }
            const auto report = reclaimer.reclaimJournal(snapshot, advanced.journal, options,
                LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1));
            retired += report.retired_values; ++windows; complete = advanced.journal.manifest_scan_exhausted;
            auto resetPin = metadata_->openSnapshot(); LargeValueMaintenanceJournal reset; reset.scan_generation = resetPin.superblock().generation;
            if (!complete) reset.manifest_after_value_id = advanced.journal.manifest_after_value_id;
            (void)store.saveAfterScan(resetPin, reset);
        }
    }
    EXPECT_TRUE(complete); EXPECT_EQ(windows, 6U); EXPECT_EQ(retired, 1U);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 2}), expected); EXPECT_EQ(roots.readEntity(old, {NODE, 2}), previous);
    size_t manifests = 0; metadata_->visitLargeValueManifestLocations(metadata_->openSnapshot(), [&](const auto &) { ++manifests; }); EXPECT_EQ(manifests, 5U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, OwnerDeletionInvalidatesOnlyCanonicalJournalEvidenceAndRetainsTheOtherOwner) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    {
        graph::utils::OwnedAllocationScope scope(memory); auto source = metadata_->openSnapshot();
        LargeValueMaintenanceJournal initial; initial.scan_generation = source.superblock().generation;
        (void)store.saveAfterScan(source, initial); source.release(); auto old = metadata_->openSnapshot(); const auto persisted = store.load(old);
        const auto state = old.superblock(); const std::array<EntityPropertyKey, 1> removed{{{NODE, 1}}};
        NativeHybridPropertyColumnWrite deletion{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            {}, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true, removed};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(deletion, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
        auto current = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1));
        EXPECT_TRUE(metadata_->findNodePageLocationOptional(current, 2)); EXPECT_EQ(store.load(current), persisted);
        LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
        EXPECT_THROW((void)reclaimer.advanceJournal(current, *persisted, reclaimOptions()), FormatException);
        EXPECT_TRUE(metadata_->findNodePageLocationOptional(old, 1));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, ForgedPlainPhaseAndModifiedTypedAdvancementNeverPublishEvidence) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto source = metadata_->openSnapshot(); const auto before = source.superblock();
        const auto epoch = io_->dataMutationEpoch(); LargeValueMaintenanceJournal initial; initial.scan_generation = before.generation;
        auto advanced = reclaimer.advanceJournal(source, initial, reclaimOptions());
        LargeValueMaintenanceJournal fake = initial; fake.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
        fake.candidate_value_ids = {LargeValueId(1)}; fake.manifest_after_value_id = 1;
        EXPECT_THROW((void)store.saveAfterScan(source, fake), FormatException);
        EXPECT_THROW((void)reclaimer.reclaimJournal(source, fake, reclaimOptions(), LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
        advanced.journal = fake;
        EXPECT_THROW((void)store.saveAfterScan(source, advanced), FormatException);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        LargeValueMaintenanceJournal imported = initial; imported.entity_property_after_directory_id = 1;
        EXPECT_THROW((void)reclaimer.advanceJournal(source, imported, reclaimOptions()), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, LegacyRowEvidenceIsIgnoredAndResetToTheProtectedNativeIdentity) {
    seed(NODE, 2, 1); StateRecordStore states(*slotted_pages_, *metadata_); const auto before = metadata_->openSnapshot().superblock();
    LargeValueMaintenanceJournal legacy; legacy.scan_generation = Generation(before.generation.value() + 2);
    legacy.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM; legacy.candidate_value_ids = {LargeValueId(1)};
    legacy.manifest_after_value_id = 1; StateRecord record; record.state_id = 77;
    record.key = std::string(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY); record.inline_bytes = LargeValueMaintenanceJournalCodec::encode(legacy);
    const auto location = states.upsert(record, journalOptions().state_record_options); auto row = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    LargeValueStore values(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto source = metadata_->openSnapshot(); EXPECT_EQ(legacy.scan_generation, source.superblock().generation);
        EXPECT_FALSE(store.load(source)); const auto epoch = io_->dataMutationEpoch();
        EXPECT_THROW((void)reclaimer.advanceJournal(source, legacy, reclaimOptions()), FormatException);
        EXPECT_THROW((void)reclaimer.reclaimJournal(source, legacy, reclaimOptions(), LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1)), FormatException);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        LargeValueMaintenanceJournal reset; reset.scan_generation = source.superblock().generation; (void)store.saveAfterScan(source, reset);
        auto current = metadata_->openSnapshot(); EXPECT_TRUE(store.load(current)); EXPECT_EQ(metadata_->findStateLocation(current, 77), location);
        EXPECT_EQ(states.read(current, 77).key, RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
        EXPECT_EQ(states.read(row, 77).key, RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, ExhaustingPinnedJournalFramesMovesThePoolAndReleasesItOnlyAfterPinsDrain) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    std::vector<MetadataSnapshotPin> pins; SegmentId originalPool; Generation originalGeneration;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        for (unsigned step = 0; step < 33; ++step) {
            auto source = metadata_->openSnapshot(); LargeValueMaintenanceJournal journal; journal.scan_generation = source.superblock().generation;
            (void)store.saveAfterScan(source, journal); pins.push_back(metadata_->openSnapshot());
        }
        const auto state = metadata_->findStateLocation(pins.front(), 77);
        const auto firstRoute = metadata_->findPageLocation(pins.front(), state.page_id);
        originalPool = firstRoute.segment_id; originalGeneration = firstRoute.generation;
        const auto current = metadata_->openSnapshot(); const auto lastRoute = metadata_->findPageLocation(current, state.page_id);
        EXPECT_NE(lastRoute.segment_id, originalPool); EXPECT_EQ(store.load(pins.front())->scan_generation, pins.front().superblock().generation);
        EXPECT_EQ(metadata_->findCatalog(current, originalPool).state, SegmentState::SGS_RETIRED);
    }
    const std::array<CatalogSegmentRetirementRequest, 1> requests{{{originalPool, originalGeneration}}};
    const auto before = metadata_->openSnapshot().superblock();
    EXPECT_FALSE(metadata_->tryReleaseRetiredCatalogSegments(requests, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    pins.clear(); ASSERT_TRUE(metadata_->tryReleaseRetiredCatalogSegments(requests, LogSequenceNumber(before.checkpoint_lsn.value() + 1)));
    EXPECT_TRUE(store.load(metadata_->openSnapshot()));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, PersistedNativeOwnerProgressResumesAfterAllOriginalStoresClose) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); Generation persisted;
    {
        LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_); seedMixed(values, roots, memory);
        StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
        LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
        graph::utils::OwnedAllocationScope scope(memory); auto source = metadata_->openSnapshot(); LargeValueMaintenanceJournal start;
        start.scan_generation = source.superblock().generation; auto next = reclaimer.advanceJournal(source, start, reclaimOptions());
        ASSERT_EQ(next.journal.phase, LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES);
        persisted = store.saveAfterScan(source, next).journal.scan_generation;
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(metadata_->activeSnapshotCount(), 0U);
    vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset();
    auto reopened = RelocatableMetadataStore::open(io_); SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY); StateRecordStore states(pages, *reopened);
    LargeValueMaintenanceJournalStore store(states, *reopened, journalOptions()); LargeValueReachabilityReclaimer reclaimer(values, pages, *reopened);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto source = reopened->openSnapshot(); auto journal = store.load(source);
        ASSERT_TRUE(journal); ASSERT_EQ(journal->scan_generation, persisted); ASSERT_EQ(source.superblock().generation, persisted);
        auto advanced = reclaimer.advanceJournal(source, *journal, reclaimOptions());
        EXPECT_EQ(advanced.owner_locations_scanned, 2U); EXPECT_EQ(advanced.journal.entity_property_after_directory_id, 1U);
        (void)store.saveAfterScan(source, advanced);
        EXPECT_EQ(store.load(reopened->openSnapshot())->entity_property_after_directory_id, 1U);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, InterruptedJournalAppendIsRecoveredBeforeAnUnrelatedNativePublication) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
    {
        graph::utils::OwnedAllocationScope scope(memory); auto first = metadata_->openSnapshot();
        LargeValueMaintenanceJournal journal; journal.scan_generation = first.superblock().generation;
        (void)store.saveAfterScan(first, journal); first.release(); auto old = metadata_->openSnapshot(); const auto persisted = store.load(old);
        MetadataPublishOptions interruption; interruption.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE;
        EXPECT_THROW((void)store.saveAfterScan(old, *persisted, interruption), BootstrapPublishInterrupted);
        const auto state = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(int64_t(777)))}}};
        NativeHybridPropertyColumnWrite mutation{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(mutation, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
        EXPECT_EQ(store.load(old), persisted);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyNativeJournalTest, WritableReopenRecoversInterruptedAppendBeforeWritingAnotherOwner) {
    seed(NODE, 2, 1); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); ASSERT_NO_THROW(convert(memory));
    {
        StateRecordStore states(*slotted_pages_, *metadata_); LargeValueMaintenanceJournalStore store(states, *metadata_, journalOptions());
        graph::utils::OwnedAllocationScope scope(memory); auto first = metadata_->openSnapshot();
        LargeValueMaintenanceJournal journal; journal.scan_generation = first.superblock().generation;
        (void)store.saveAfterScan(first, journal); first.release(); auto source = metadata_->openSnapshot();
        MetadataPublishOptions interruption; interruption.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE;
        EXPECT_THROW((void)store.saveAfterScan(source, *store.load(source), interruption), BootstrapPublishInterrupted);
    }
    vector_manifests_.reset(); vector_pages_.reset(); properties_.reset(); labels_.reset(); btree_pages_.reset(); slotted_pages_.reset(); entities_.reset(); metadata_.reset();
    auto reopened = RelocatableMetadataStore::open(io_);
    {
        graph::utils::OwnedAllocationScope scope(memory); const auto state = reopened->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{{{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(int64_t(888)))}}};
        NativeHybridPropertyColumnWrite mutation{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)reopened->mutateHybridPropertyColumns(mutation, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
    }
    EXPECT_EQ(memory->used(), 0U);
}
#endif
